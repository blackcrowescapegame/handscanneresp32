#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>

#include "app_config.h"
#include "audio_engine.h"
#include "displays_config.h"
#include "embedded_assets.h"
#include "gt911.h"
#include "network_client.h"

namespace {
constexpr int16_t kWidth = 800;
constexpr int16_t kHeight = 1280;
constexpr int16_t kTouchRadius = 35;
constexpr int16_t kButtonRadius = 28;
constexpr uint32_t kScanPeriodMs = 3000;
constexpr uint32_t kScanFrameMs = 50;
constexpr uint32_t kResultDelayMs = 500;
constexpr uint32_t kDeniedDurationMs = 2000;
constexpr uint32_t kFadeHalfMs = 2000;
constexpr uint32_t kFadeFrameMs = 100;

constexpr uint8_t kCorrectOrder[5] = {3, 2, 4, 5, 1};

struct TouchTarget {
    int16_t x;
    int16_t y;
};

constexpr TouchTarget kTargets[5] = {
    {109, 651},  // Thumb
    {300, 309},
    {428, 287},
    {536, 329},
    {698, 471},
};

enum class ScreenMode : uint8_t {
    Normal,
    Blackout,
    Hint,
    SuccessFade,
};

enum class ResultState : uint8_t {
    None,
    Granted,
    Denied,
};

Arduino_ESP32DSIPanel dsiPanel(
    display_cfg.hsync_pulse_width,
    display_cfg.hsync_back_porch,
    display_cfg.hsync_front_porch,
    display_cfg.vsync_pulse_width,
    display_cfg.vsync_back_porch,
    display_cfg.vsync_front_porch,
    display_cfg.prefer_speed,
    display_cfg.lane_bit_rate);

Arduino_DSI_Display display(
    display_cfg.width,
    display_cfg.height,
    &dsiPanel,
    HANDSCANNER_DISPLAY_ROTATION,
    false,
    display_cfg.lcd_rst,
    display_cfg.init_cmds,
    display_cfg.init_cmds_size);

esp_lcd_touch_handle_t touch = nullptr;
QueueHandle_t remoteCommands = nullptr;
uint16_t *blendBuffer = nullptr;

ScreenMode screenMode = ScreenMode::Normal;
ResultState resultState = ResultState::None;
uint8_t userInput[5] = {};
bool selected[5] = {};
uint8_t pressCount = 0;
bool solved = false;
bool touchWasDown = false;
bool otaUiSuspended = false;
bool accessCheckPending = false;
uint32_t otaSuspendedAt = 0;
uint32_t accessCheckAt = 0;
uint32_t resultShownAt = 0;
uint32_t successStartedAt = 0;
uint32_t lastFadeFrameAt = 0;
uint32_t lastScanFrameAt = 0;
int16_t previousScanY = -1;

const uint16_t *basePixels() { return reinterpret_cast<const uint16_t *>(base_start); }
const uint16_t *skullPixels() { return reinterpret_cast<const uint16_t *>(skull_start); }
const uint16_t *hintPixels() { return reinterpret_cast<const uint16_t *>(hint_start); }

void drawImage(const uint16_t *pixels) {
    display.draw16bitRGBBitmap(0, 0, const_cast<uint16_t *>(pixels), kWidth, kHeight);
    display.flush();
}

void drawCenteredText(const char *text, int16_t centerY, uint16_t color, uint8_t size) {
    int16_t x1, y1;
    uint16_t width, height;
    display.setTextSize(size);
    display.setTextColor(color);
    display.getTextBounds(text, 0, 0, &x1, &y1, &width, &height);
    display.setCursor((kWidth - static_cast<int16_t>(width)) / 2,
                      centerY - static_cast<int16_t>(height) / 2);
    display.print(text);
}

void drawButtons() {
    constexpr uint16_t purple = 0x8010;
    for (uint8_t i = 0; i < 5; ++i) {
        display.fillCircle(kTargets[i].x, kTargets[i].y, kButtonRadius, purple);
        if (selected[i]) {
            char value[2] = {static_cast<char>('0' + [&]() {
                                 for (uint8_t p = 0; p < pressCount; ++p) {
                                     if (userInput[p] == i + 1) return p + 1;
                                 }
                                 return 0;
                             }()), '\0'};
            int16_t x1, y1;
            uint16_t width, height;
            display.setTextSize(4);
            display.setTextColor(RGB565_WHITE);
            display.getTextBounds(value, 0, 0, &x1, &y1, &width, &height);
            display.setCursor(kTargets[i].x - width / 2, kTargets[i].y - height / 2);
            display.print(value);
        }
    }
}

void drawResult() {
    if (resultState == ResultState::Granted) {
        drawCenteredText("ACCESS GRANTED", 1100, RGB565_WHITE, 4);
    } else if (resultState == ResultState::Denied) {
        drawCenteredText("ACCESS DENIED", 1100, RGB565_RED, 4);
    }
}

void drawNormalScreen() {
    drawImage(basePixels());
    drawButtons();
    drawResult();
    display.flush();
    previousScanY = -1;
}

void resetGame(bool playRiser) {
    pressCount = 0;
    memset(userInput, 0, sizeof(userInput));
    memset(selected, 0, sizeof(selected));
    solved = false;
    accessCheckPending = false;
    resultState = ResultState::None;
    screenMode = ScreenMode::Normal;
    drawNormalScreen();
    if (playRiser) audioPlay(riserClip(), 3);
}

void applyRemoteCommand(RemoteCommand command) {
    switch (command) {
        case RemoteCommand::Reset:
            Serial.println("Game: remote reset");
            resetGame(true);
            break;
        case RemoteCommand::Blackout:
            Serial.println("Game: blackout");
            screenMode = ScreenMode::Blackout;
            display.fillScreen(RGB565_BLACK);
            display.flush();
            break;
        case RemoteCommand::ShowHint:
            Serial.println("Game: showing Belial hint");
            screenMode = ScreenMode::Hint;
            drawImage(hintPixels());
            break;
    }
}

bool transformTouch(uint16_t rawX, uint16_t rawY, int16_t &x, int16_t &y) {
#if HANDSCANNER_DISPLAY_ROTATION == 0
    x = rawX;
    y = rawY;
#elif HANDSCANNER_DISPLAY_ROTATION == 1
    x = rawY;
    y = display_cfg.width - 1 - rawX;
#elif HANDSCANNER_DISPLAY_ROTATION == 2
    x = display_cfg.width - 1 - rawX;
    y = display_cfg.height - 1 - rawY;
#elif HANDSCANNER_DISPLAY_ROTATION == 3
    x = display_cfg.height - 1 - rawY;
    y = rawX;
#else
#error "HANDSCANNER_DISPLAY_ROTATION must be between 0 and 3"
#endif
    return x >= 0 && x < kWidth && y >= 0 && y < kHeight;
}

void onFingerPressed(uint8_t finger) {
    if (solved || screenMode != ScreenMode::Normal || finger < 1 || finger > 5 ||
        selected[finger - 1] || pressCount >= 5) {
        return;
    }

    audioPlay(fingerClip());
    selected[finger - 1] = true;
    userInput[pressCount++] = finger;
    drawButtons();
    display.flush();

    if (pressCount == 5) {
        accessCheckPending = true;
        accessCheckAt = millis() + kResultDelayMs;
    }
}

void pollTouch() {
    if (touch == nullptr || screenMode != ScreenMode::Normal || solved) return;

    uint16_t rawX[5] = {};
    uint16_t rawY[5] = {};
    uint16_t strength[5] = {};
    uint8_t count = 0;
    esp_lcd_touch_read_data(touch);
    const bool down = esp_lcd_touch_get_coordinates(touch, rawX, rawY, strength, &count, 5) && count > 0;

    if (down && !touchWasDown) {
        int16_t x, y;
        if (transformTouch(rawX[0], rawY[0], x, y)) {
            for (uint8_t i = 0; i < 5; ++i) {
                const int32_t dx = x - kTargets[i].x;
                const int32_t dy = y - kTargets[i].y;
                if (dx * dx + dy * dy <= kTouchRadius * kTouchRadius) {
                    onFingerPressed(i + 1);
                    break;
                }
            }
        }
    }
    touchWasDown = down;
}

bool correctSequence() {
    return memcmp(userInput, kCorrectOrder, sizeof(kCorrectOrder)) == 0;
}

void checkAccess() {
    accessCheckPending = false;
    networkSubmitSequence(userInput, pressCount);

    if (correctSequence()) {
        solved = true;
        resultState = ResultState::Granted;
        audioPlay(grantedClip());
        drawNormalScreen();
        screenMode = ScreenMode::SuccessFade;
        successStartedAt = millis();
        lastFadeFrameAt = 0;
        Serial.println("Game: ACCESS GRANTED");
    } else {
        resultState = ResultState::Denied;
        resultShownAt = millis();
        audioPlay(deniedClip());
        drawNormalScreen();
        Serial.println("Game: ACCESS DENIED");
    }
}

uint16_t blend565(uint16_t from, uint16_t to, uint8_t alpha) {
    const uint32_t inverse = 255 - alpha;
    const uint32_t r = ((((from >> 11) & 0x1F) * inverse) + (((to >> 11) & 0x1F) * alpha)) / 255;
    const uint32_t g = ((((from >> 5) & 0x3F) * inverse) + (((to >> 5) & 0x3F) * alpha)) / 255;
    const uint32_t b = (((from & 0x1F) * inverse) + ((to & 0x1F) * alpha)) / 255;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

void updateSuccessFade(uint32_t now) {
    if (now - lastFadeFrameAt < kFadeFrameMs) return;
    lastFadeFrameAt = now;

    const uint32_t elapsed = now - successStartedAt;
    if (elapsed >= kFadeHalfMs * 2) {
        screenMode = ScreenMode::Normal;
        drawNormalScreen();
        return;
    }

    uint8_t alpha;
    if (elapsed < kFadeHalfMs) {
        alpha = static_cast<uint8_t>((elapsed * 255UL) / kFadeHalfMs);
    } else {
        alpha = static_cast<uint8_t>(((kFadeHalfMs * 2 - elapsed) * 255UL) / kFadeHalfMs);
    }

    if (blendBuffer == nullptr) {
        drawImage(alpha >= 128 ? skullPixels() : basePixels());
        return;
    }

    constexpr size_t pixelCount = static_cast<size_t>(kWidth) * kHeight;
    const uint16_t *base = basePixels();
    const uint16_t *skull = skullPixels();
    for (size_t i = 0; i < pixelCount; ++i) {
        blendBuffer[i] = blend565(base[i], skull[i], alpha);
    }
    display.draw16bitRGBBitmap(0, 0, blendBuffer, kWidth, kHeight);
    display.flush();
}

void updateScan(uint32_t now) {
    if (now - lastScanFrameAt < kScanFrameMs) return;
    lastScanFrameAt = now;
    const int16_t scanY = static_cast<int16_t>(((now % kScanPeriodMs) * kHeight) / kScanPeriodMs);

    if (previousScanY >= 0) {
        const int16_t top = max<int16_t>(0, previousScanY - 5);
        const int16_t bottom = min<int16_t>(kHeight, previousScanY + 6);
        const uint16_t *source = basePixels() + static_cast<size_t>(top) * kWidth;
        display.draw16bitRGBBitmap(0, top, const_cast<uint16_t *>(source), kWidth, bottom - top);
    }

    display.drawFastHLine(0, max<int16_t>(0, scanY - 3), kWidth, 0x03E0);
    display.drawFastHLine(0, max<int16_t>(0, scanY - 1), kWidth, 0x07E0);
    display.drawFastHLine(0, scanY, kWidth, 0x7FE0);
    display.drawFastHLine(0, min<int16_t>(kHeight - 1, scanY + 2), kWidth, 0x07E0);
    drawButtons();
    drawResult();
    display.flush();
    previousScanY = scanY;
}

void suspendUiForOta() {
    otaUiSuspended = true;
    otaSuspendedAt = millis();
    touchWasDown = false;
    audioStop();
    display.fillScreen(RGB565_BLACK);
    display.flush();
    networkConfirmOtaUiReady();
}

void resumeUiAfterOtaFailure() {
    const uint32_t now = millis();
    const uint32_t pausedFor = now - otaSuspendedAt;
    if (accessCheckPending) accessCheckAt += pausedFor;
    if (resultState == ResultState::Denied) resultShownAt += pausedFor;
    if (screenMode == ScreenMode::SuccessFade) successStartedAt += pausedFor;
    lastFadeFrameAt = now;
    lastScanFrameAt = now;
    otaUiSuspended = false;

    switch (screenMode) {
        case ScreenMode::Normal:
        case ScreenMode::SuccessFade:
            drawNormalScreen();
            break;
        case ScreenMode::Blackout:
            display.fillScreen(RGB565_BLACK);
            display.flush();
            break;
        case ScreenMode::Hint:
            drawImage(hintPixels());
            break;
    }
    Serial.println("OTA: game resumed after failed/cancelled update");
}
}  // namespace

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("Native Handscanner starting (Waveshare SKU 33150)");

    if ((base_end - base_start) != kWidth * kHeight * 2 ||
        (skull_end - skull_start) != kWidth * kHeight * 2 ||
        (hint_end - hint_start) != kWidth * kHeight * 2) {
        Serial.println("Fatal: embedded UI asset has the wrong size");
        while (true) delay(1000);
    }

    audioBegin();

    if (!display_cfg_prepare()) {
        Serial.println("Fatal: display preparation failed");
        while (true) delay(1000);
    }
    display_cfg_backlight(true);
    if (!display.begin()) {
        Serial.println("Fatal: display initialization failed");
        while (true) delay(1000);
    }

    DEV_I2C_Port i2c = DEV_I2C_Init();
    touch = touch_gt911_init(i2c);
    if (touch == nullptr) {
        Serial.println("Warning: GT9271 touch was not detected");
    }

    blendBuffer = static_cast<uint16_t *>(heap_caps_malloc(
        static_cast<size_t>(kWidth) * kHeight * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (blendBuffer == nullptr) {
        Serial.println("Warning: no fade buffer; using cut transitions");
    }

    remoteCommands = xQueueCreate(8, sizeof(RemoteCommand));
    networkBegin(remoteCommands);
    drawNormalScreen();
    Serial.println("Handscanner ready");
}

void loop() {
    // The OTA callback waits for this acknowledgement before receiving firmware,
    // ensuring no display, audio, touch, or flash-backed game work overlaps it.
    if (networkOtaInProgress()) {
        if (!otaUiSuspended) suspendUiForOta();
        delay(20);
        return;
    }
    if (otaUiSuspended) resumeUiAfterOtaFailure();

    const uint32_t now = millis();

    RemoteCommand command;
    while (remoteCommands != nullptr && xQueueReceive(remoteCommands, &command, 0) == pdTRUE) {
        applyRemoteCommand(command);
    }

    pollTouch();

    if (accessCheckPending && static_cast<int32_t>(now - accessCheckAt) >= 0) {
        checkAccess();
    }

    if (screenMode == ScreenMode::SuccessFade) {
        updateSuccessFade(now);
    } else if (screenMode == ScreenMode::Normal) {
        if (resultState == ResultState::Denied && now - resultShownAt >= kDeniedDurationMs) {
            resetGame(false);
        } else {
            updateScan(now);
        }
    }

    delay(5);
}
