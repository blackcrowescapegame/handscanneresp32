#include "audio_engine.h"

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <esp_check.h>

#include "app_config.h"
#include "es8311.h"

namespace {
constexpr gpio_num_t kAmpEnable = GPIO_NUM_53;
constexpr gpio_num_t kMclk = GPIO_NUM_13;
constexpr gpio_num_t kBclk = GPIO_NUM_12;
constexpr gpio_num_t kLrck = GPIO_NUM_10;
constexpr gpio_num_t kDataOut = GPIO_NUM_9;
constexpr uint32_t kSampleRate = 16000;
constexpr size_t kMonoSamplesPerFrame = 128;
constexpr i2s_port_t kI2sPort = I2S_NUM_0;

struct AudioRequest {
    const uint8_t *data;
    size_t length;
    uint8_t repeats;
};

QueueHandle_t requestQueue = nullptr;
bool ready = false;

esp_err_t initCodec() {
    es8311_handle_t codec = es8311_create(0, ES8311_ADDRESS_0);
    ESP_RETURN_ON_FALSE(codec != nullptr, ESP_FAIL, "audio", "ES8311 allocation failed");

    const es8311_clock_config_t clock = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = static_cast<int>(kSampleRate * 256),
        .sample_frequency = static_cast<int>(kSampleRate),
    };
    ESP_RETURN_ON_ERROR(es8311_init(codec, &clock, ES8311_RESOLUTION_16,
                                    ES8311_RESOLUTION_16),
                        "audio", "ES8311 init failed");
    ESP_RETURN_ON_ERROR(es8311_microphone_config(codec, false), "audio",
                        "ES8311 microphone config failed");
    ESP_RETURN_ON_ERROR(es8311_voice_volume_set(codec, HANDSCANNER_AUDIO_VOLUME, nullptr),
                        "audio", "ES8311 volume failed");
    return ESP_OK;
}

void audioTask(void *) {
    AudioRequest request{};
    AudioRequest replacement{};
    int16_t stereo[kMonoSamplesPerFrame * 2];

    for (;;) {
        if (xQueueReceive(requestQueue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        while (request.repeats > 0) {
            size_t offset = 0;
            bool replaced = false;
            while (offset + sizeof(int16_t) <= request.length) {
                if (xQueueReceive(requestQueue, &replacement, 0) == pdTRUE) {
                    request = replacement;
                    replaced = true;
                    i2s_zero_dma_buffer(kI2sPort);
                    break;
                }

                const size_t remainingSamples = (request.length - offset) / sizeof(int16_t);
                const size_t count = min(remainingSamples, kMonoSamplesPerFrame);
                for (size_t i = 0; i < count; ++i) {
                    int16_t sample;
                    memcpy(&sample, request.data + offset + i * sizeof(int16_t), sizeof(sample));
                    stereo[i * 2] = sample;
                    stereo[i * 2 + 1] = sample;
                }

                size_t written = 0;
                i2s_write(kI2sPort, stereo, count * 2 * sizeof(int16_t), &written,
                          portMAX_DELAY);
                offset += count * sizeof(int16_t);
            }

            if (!replaced) {
                --request.repeats;
            }
        }
        i2s_zero_dma_buffer(kI2sPort);
    }
}
}  // namespace

bool audioBegin() {
    pinMode(kAmpEnable, OUTPUT);
    digitalWrite(kAmpEnable, HIGH);

    // Initialize Arduino's I2C bus first. The Waveshare touch helper then
    // discovers and reuses this same bus for the GT9271.
    if (!Wire.begin(7, 8, 100000)) {
        Serial.println("Audio: I2C initialization failed");
        return false;
    }

    const i2s_config_t i2sConfig = {
        .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = kSampleRate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 128,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        .bits_per_chan = I2S_BITS_PER_CHAN_16BIT,
    };
    const i2s_pin_config_t pinConfig = {
        .mck_io_num = kMclk,
        .bck_io_num = kBclk,
        .ws_io_num = kLrck,
        .data_out_num = kDataOut,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };

    if (i2s_driver_install(kI2sPort, &i2sConfig, 0, nullptr) != ESP_OK ||
        i2s_set_pin(kI2sPort, &pinConfig) != ESP_OK) {
        Serial.println("Audio: I2S initialization failed");
        return false;
    }
    i2s_zero_dma_buffer(kI2sPort);

    if (initCodec() != ESP_OK) {
        Serial.println("Audio: ES8311 initialization failed");
        i2s_driver_uninstall(kI2sPort);
        return false;
    }

    requestQueue = xQueueCreate(1, sizeof(AudioRequest));
    if (requestQueue == nullptr) {
        Serial.println("Audio: queue allocation failed");
        return false;
    }
    if (xTaskCreate(audioTask, "handscanner-audio", 4096, nullptr, 2, nullptr) != pdPASS) {
        Serial.println("Audio: task creation failed");
        return false;
    }
    ready = true;
    Serial.println("Audio: ready");
    return true;
}

void audioPlay(EmbeddedClip clip, uint8_t repeats) {
    if (!ready || clip.data == nullptr || clip.length < sizeof(int16_t) || repeats == 0) {
        return;
    }
    const AudioRequest request{clip.data, clip.length, repeats};
    xQueueOverwrite(requestQueue, &request);
}

void audioStop() {
    if (!ready || requestQueue == nullptr) return;
    const AudioRequest stopRequest{nullptr, 0, 0};
    xQueueOverwrite(requestQueue, &stopRequest);
    i2s_zero_dma_buffer(kI2sPort);
}
