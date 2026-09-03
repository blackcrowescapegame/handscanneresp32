#include "network_client.h"

#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <time.h>

#include "app_config.h"

namespace {
struct SequenceMessage {
    uint8_t values[5];
    uint8_t count;
};

QueueHandle_t sequenceQueue = nullptr;
QueueHandle_t commandQueue = nullptr;

String lastUserInput;
String lastChanged;
String lastReported;
String lastUpdated;
bool otaStarted = false;
bool otaInProgress = false;
uint8_t lastOtaPercent = 255;

void startOta() {
    if (otaStarted || strlen(HANDSCANNER_OTA_PASSWORD) == 0) return;

    ArduinoOTA.setHostname(HANDSCANNER_OTA_HOSTNAME);
    ArduinoOTA.setPort(HANDSCANNER_OTA_PORT);
    ArduinoOTA.setPassword(HANDSCANNER_OTA_PASSWORD);
    ArduinoOTA.setRebootOnSuccess(true);
    ArduinoOTA.onStart([]() {
        otaInProgress = true;
        lastOtaPercent = 255;
        Serial.println("OTA: update started");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        if (total == 0) return;
        const uint8_t percent = static_cast<uint8_t>((progress * 100ULL) / total);
        if (percent != lastOtaPercent && percent % 10 == 0) {
            lastOtaPercent = percent;
            Serial.printf("OTA: %u%%\n", percent);
        }
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("OTA: update complete; rebooting");
    });
    ArduinoOTA.onError([](ota_error_t error) {
        otaInProgress = false;
        Serial.printf("OTA: failed (%u)\n", static_cast<unsigned int>(error));
    });
    ArduinoOTA.begin();
    otaStarted = true;
    Serial.printf("OTA: ready at %s.local:%u\n", HANDSCANNER_OTA_HOSTNAME,
                  static_cast<unsigned int>(HANDSCANNER_OTA_PORT));
}

String isoTimestamp() {
    time_t now = time(nullptr);
    struct tm utc{};
    if (now > 1700000000 && gmtime_r(&now, &utc) != nullptr) {
        char value[32];
        strftime(value, sizeof(value), "%Y-%m-%dT%H:%M:%SZ", &utc);
        return String(value);
    }
    return String("1970-01-01T00:00:") + String((millis() / 1000) % 60) + "Z";
}

String sequenceJson(const SequenceMessage &message) {
    String result("[");
    for (uint8_t i = 0; i < message.count; ++i) {
        if (i > 0) result += ',';
        result += String(message.values[i]);
    }
    result += ']';
    return result;
}

bool ensureWifi() {
    if (strlen(HANDSCANNER_WIFI_SSID) == 0) {
        return false;
    }
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }

    Serial.printf("WiFi: connecting to %s\n", HANDSCANNER_WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(HANDSCANNER_WIFI_SSID, HANDSCANNER_WIFI_PASSWORD);
    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi: connection timed out");
        return false;
    }

    Serial.printf("WiFi: connected, IP %s\n", WiFi.localIP().toString().c_str());
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    return true;
}

void sendSequence(const SequenceMessage &message) {
    if (strlen(HANDSCANNER_HA_EVENT_URL) == 0) return;

    const String currentState = sequenceJson(message);
    const String changed = isoTimestamp();
    const String reported = changed;
    const String updated = changed;

    String payload;
    payload.reserve(600);
    payload = "{\"event_type\":\"state_changed\",\"entity_id\":\"custom_sensor.handscanner\",\"event\":{";
    payload += "\"entity_id\":\"custom_sensor.handscanner\",\"old_state\":{";
    payload += "\"entity_id\":\"custom_sensor.handscanner\",\"state\":\"" + lastUserInput;
    payload += "\",\"last_changed\":\"" + lastChanged + "\",\"last_reported\":\"";
    payload += lastReported + "\",\"last_updated\":\"" + lastUpdated + "\"},\"new_state\":{";
    payload += "\"entity_id\":\"custom_sensor.handscanner\",\"state\":\"" + currentState;
    payload += "\",\"last_changed\":\"" + changed + "\",\"last_reported\":\"";
    payload += reported + "\",\"last_updated\":\"" + updated + "\"}}}";

    HTTPClient http;
    http.setConnectTimeout(HANDSCANNER_HTTP_TIMEOUT_MS);
    http.setTimeout(HANDSCANNER_HTTP_TIMEOUT_MS);
    if (!http.begin(HANDSCANNER_HA_EVENT_URL)) {
        Serial.println("API: invalid Home Assistant URL");
        return;
    }
    http.addHeader("Content-Type", "application/json");
    if (strlen(HANDSCANNER_BEARER_TOKEN) > 0) {
        http.addHeader("Authorization", String("Bearer ") + HANDSCANNER_BEARER_TOKEN);
    }
    const int status = http.POST(payload);
    Serial.printf("API: Home Assistant POST -> %d\n", status);
    http.end();

    lastUserInput = currentState;
    lastChanged = changed;
    lastReported = reported;
    lastUpdated = updated;
}

void pollRemoteCommand() {
    if (strlen(HANDSCANNER_RESET_URL) == 0) return;

    HTTPClient http;
    http.setConnectTimeout(HANDSCANNER_HTTP_TIMEOUT_MS);
    http.setTimeout(HANDSCANNER_HTTP_TIMEOUT_MS);
    if (!http.begin(HANDSCANNER_RESET_URL)) return;
    const int status = http.GET();
    if (status >= 200 && status < 300) {
        String body = http.getString();
        body.trim();
        const int value = body.toInt();
        if (value >= 1 && value <= 3) {
            const RemoteCommand command = static_cast<RemoteCommand>(value);
            xQueueSend(commandQueue, &command, 0);
            Serial.printf("API: remote command %d\n", value);
        }
    } else if (status < 0) {
        Serial.printf("API: reset poll failed (%d)\n", status);
    }
    http.end();
}

void networkTask(void *) {
    uint32_t lastConnectAttempt = millis() - 10000;
    uint32_t lastPoll = millis();
    SequenceMessage message{};

    for (;;) {
        if (WiFi.status() != WL_CONNECTED && millis() - lastConnectAttempt >= 10000) {
            lastConnectAttempt = millis();
            ensureWifi();
        }

        if (WiFi.status() == WL_CONNECTED) {
            startOta();
            if (otaStarted) ArduinoOTA.handle();
            if (otaInProgress) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            while (xQueueReceive(sequenceQueue, &message, 0) == pdTRUE) {
                sendSequence(message);
            }
            if (millis() - lastPoll >= HANDSCANNER_RESET_POLL_MS) {
                lastPoll = millis();
                pollRemoteCommand();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
}  // namespace

bool networkBegin(QueueHandle_t remoteCommandQueue) {
    commandQueue = remoteCommandQueue;
    sequenceQueue = xQueueCreate(4, sizeof(SequenceMessage));
    if (commandQueue == nullptr || sequenceQueue == nullptr) return false;
    if (strlen(HANDSCANNER_WIFI_SSID) == 0) {
        Serial.println("WiFi: disabled; copy include/secrets.example.h to include/secrets.h and configure it");
    }
    return xTaskCreate(networkTask, "handscanner-network", 8192, nullptr, 1, nullptr) == pdPASS;
}

void networkSubmitSequence(const uint8_t *values, size_t count) {
    if (sequenceQueue == nullptr || values == nullptr) return;
    SequenceMessage message{};
    message.count = min(count, sizeof(message.values));
    memcpy(message.values, values, message.count);
    xQueueSend(sequenceQueue, &message, 0);
}
