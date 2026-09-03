#include "network_client.h"

#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <ping/ping_sock.h>
#include <time.h>

#include "app_config.h"

namespace {
constexpr uint32_t kWifiReconnectIntervalMs = 10000;
constexpr uint32_t kGatewayPingIntervalMs = 10000;
constexpr uint32_t kGatewayPingTimeoutMs = 1000;
constexpr uint8_t kFailedProbesBeforeRadioReset = 3;
constexpr uint32_t kOtaUiReadyTimeoutMs = 3000;

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
volatile bool otaInProgress = false;
volatile bool otaUiReady = false;
uint8_t lastOtaPercent = 255;
uint8_t lastOtaError = 0;
uint8_t lastUpdateError = 0;
String lastUpdateErrorMessage = "none";
const char *otaState = "disabled";
bool healthApiStarted = false;
bool healthApiConfigured = false;
bool wifiWasConnected = false;
volatile uint8_t failedGatewayProbes = 0;
volatile bool radioReconnectRequested = false;
uint32_t radioReconnectCount = 0;
esp_ping_handle_t gatewayPing = nullptr;
WebServer healthServer(HANDSCANNER_HEALTH_API_PORT);

void onGatewayPingSuccess(esp_ping_handle_t ping, void *) {
    uint32_t elapsedMs = 0;
    esp_ping_get_profile(ping, ESP_PING_PROF_TIMEGAP, &elapsedMs, sizeof(elapsedMs));
    const uint8_t previousFailures = failedGatewayProbes;
    failedGatewayProbes = 0;
    if (previousFailures > 0) {
        Serial.printf("WiFi: gateway probe recovered after %u failure(s), %lu ms\n",
                      previousFailures, static_cast<unsigned long>(elapsedMs));
    }
}

void onGatewayPingTimeout(esp_ping_handle_t, void *) {
    uint8_t failures = failedGatewayProbes;
    if (failures < UINT8_MAX) ++failures;
    failedGatewayProbes = failures;
    Serial.printf("WiFi: gateway probe failed (%u/%u)\n", failures,
                  kFailedProbesBeforeRadioReset);
    if (failures >= kFailedProbesBeforeRadioReset) {
        radioReconnectRequested = true;
    }
}

void onGatewayPingEnd(esp_ping_handle_t, void *) {}

void stopGatewayPing() {
    if (gatewayPing == nullptr) return;
    esp_ping_stop(gatewayPing);
    esp_ping_delete_session(gatewayPing);
    gatewayPing = nullptr;
    failedGatewayProbes = 0;
}

void startGatewayPing() {
    if (gatewayPing != nullptr || WiFi.status() != WL_CONNECTED) return;

    const IPAddress gateway = WiFi.gatewayIP();
    if (gateway == IPAddress(0, 0, 0, 0)) {
        Serial.println("WiFi: gateway probe unavailable (no gateway address)");
        return;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = ESP_PING_COUNT_INFINITE;
    config.interval_ms = kGatewayPingIntervalMs;
    config.timeout_ms = kGatewayPingTimeoutMs;
    config.data_size = 32;
    IP_ADDR4(&config.target_addr, gateway[0], gateway[1], gateway[2], gateway[3]);

    const esp_ping_callbacks_t callbacks = {
        .cb_args = nullptr,
        .on_ping_success = onGatewayPingSuccess,
        .on_ping_timeout = onGatewayPingTimeout,
        .on_ping_end = onGatewayPingEnd,
    };
    const esp_err_t created = esp_ping_new_session(&config, &callbacks, &gatewayPing);
    if (created != ESP_OK || esp_ping_start(gatewayPing) != ESP_OK) {
        Serial.printf("WiFi: could not start gateway probe (%d)\n", created);
        stopGatewayPing();
        return;
    }
    Serial.printf("WiFi: probing gateway %s every %lu seconds\n", gateway.toString().c_str(),
                  static_cast<unsigned long>(kGatewayPingIntervalMs / 1000));
}

void stopNetworkServices() {
    stopGatewayPing();
    if (otaStarted && !otaInProgress) {
        ArduinoOTA.end();
        otaStarted = false;
        otaState = strlen(HANDSCANNER_OTA_PASSWORD) > 0 ? "waiting_for_wifi" : "disabled";
    }
    if (healthApiStarted) {
        healthServer.stop();
        healthApiStarted = false;
    }
}

void configureStation() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
}

void beginStationConnection() {
    Serial.printf("WiFi: connecting to %s\n", HANDSCANNER_WIFI_SSID);
    WiFi.begin(HANDSCANNER_WIFI_SSID, HANDSCANNER_WIFI_PASSWORD);
}

void reconnectWifiRadio() {
    if (otaInProgress) return;

    radioReconnectRequested = false;
    ++radioReconnectCount;
    Serial.printf("WiFi: resetting radio after %u failed gateway probes (attempt %lu)\n",
                  kFailedProbesBeforeRadioReset,
                  static_cast<unsigned long>(radioReconnectCount));
    stopNetworkServices();
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_OFF);
    vTaskDelay(pdMS_TO_TICKS(250));
    configureStation();
    beginStationConnection();
}

void wifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        const wifi_err_reason_t reason =
            static_cast<wifi_err_reason_t>(info.wifi_sta_disconnected.reason);
        Serial.printf("WiFi: disconnected, reason %u (%s)\n", static_cast<unsigned int>(reason),
                      WiFi.disconnectReasonName(reason));
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        Serial.printf("WiFi: connected, IP %s, gateway %s\n",
                      WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str());
    }
}

void startHealthApi() {
    if (healthApiStarted) return;

    if (!healthApiConfigured) {
        healthServer.on("/api/health", HTTP_GET, []() {
            const esp_partition_t *running = esp_ota_get_running_partition();
            const char *partition = running != nullptr ? running->label : "unknown";

            String body;
            body.reserve(480);
            body = "{\"status\":\"ok\",\"state\":\"";
            body += otaInProgress ? "ota_updating" : "ready";
            body += "\",\"version\":\"" HANDSCANNER_FIRMWARE_VERSION "\"";
            body += ",\"uptime_ms\":" + String(millis());
            body += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
            body += ",\"rssi_dbm\":" + String(WiFi.RSSI());
            body += ",\"gateway\":\"" + WiFi.gatewayIP().toString() + "\"";
            body += ",\"gateway_probe_failures\":" + String(failedGatewayProbes);
            body += ",\"wifi_radio_reconnects\":" + String(radioReconnectCount);
            body += ",\"wifi_sleep_enabled\":false";
            body += ",\"free_heap_bytes\":" + String(ESP.getFreeHeap());
            body += ",\"ota_enabled\":";
            body += strlen(HANDSCANNER_OTA_PASSWORD) > 0 ? "true" : "false";
            body += ",\"ota_state\":\"" + String(otaState) + "\"";
            body += ",\"ota_error\":" + String(lastOtaError);
            body += ",\"ota_update_error\":" + String(lastUpdateError);
            body += ",\"ota_update_error_message\":\"" + lastUpdateErrorMessage + "\"";
            body += ",\"ota_partition\":\"" + String(partition) + "\"}";

            healthServer.sendHeader("Cache-Control", "no-store");
            healthServer.sendHeader("Access-Control-Allow-Origin", "*");
            healthServer.send(200, "application/json", body);
        });
        healthApiConfigured = true;
    }
    healthServer.begin();
    healthApiStarted = true;
    Serial.printf("HTTP API: http://%s/api/health\n", WiFi.localIP().toString().c_str());
}

void startOta() {
    if (otaStarted || strlen(HANDSCANNER_OTA_PASSWORD) == 0) return;

    ArduinoOTA.setHostname(HANDSCANNER_OTA_HOSTNAME);
    ArduinoOTA.setPort(HANDSCANNER_OTA_PORT);
    ArduinoOTA.setPassword(HANDSCANNER_OTA_PASSWORD);
    ArduinoOTA.setRebootOnSuccess(true);
    ArduinoOTA.onStart([]() {
        otaInProgress = true;
        otaUiReady = false;
        otaState = "updating";
        lastOtaError = 0;
        lastUpdateError = 0;
        lastUpdateErrorMessage = "none";
        lastOtaPercent = 255;
        radioReconnectRequested = false;
        stopGatewayPing();
        Serial.println("OTA: update requested; pausing game and blanking display");

        const uint32_t waitStarted = millis();
        while (!otaUiReady && millis() - waitStarted < kOtaUiReadyTimeoutMs) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (otaUiReady) {
            Serial.println("OTA: display is black and game activity is paused");
        } else {
            Serial.println("OTA: warning - timed out waiting for display pause");
        }
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
        otaState = "success";
        Serial.println("OTA: update complete; rebooting");
    });
    ArduinoOTA.onError([](ota_error_t error) {
        otaInProgress = false;
        otaUiReady = false;
        otaState = "failed";
        lastOtaError = static_cast<uint8_t>(error);
        lastUpdateError = Update.getError();
        lastUpdateErrorMessage = Update.errorString();
        Serial.printf("OTA: failed (%u), updater error %u (%s)\n",
                      static_cast<unsigned int>(error),
                      static_cast<unsigned int>(lastUpdateError),
                      lastUpdateErrorMessage.c_str());
    });
    ArduinoOTA.begin();
    otaStarted = true;
    otaState = "ready";
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
    uint32_t lastConnectAttempt = millis();
    uint32_t lastPoll = millis();
    SequenceMessage message{};

    WiFi.onEvent(wifiEvent);
    configureStation();
    beginStationConnection();

    for (;;) {
        bool connected = WiFi.status() == WL_CONNECTED;
        if (connected != wifiWasConnected) {
            wifiWasConnected = connected;
            if (connected) {
                configTime(0, 0, "pool.ntp.org", "time.nist.gov");
                startHealthApi();
                startOta();
                startGatewayPing();
            } else {
                stopNetworkServices();
            }
        }

        if (radioReconnectRequested && !otaInProgress) {
            reconnectWifiRadio();
            wifiWasConnected = false;
            lastConnectAttempt = millis();
            connected = false;
        } else if (!connected && millis() - lastConnectAttempt >= kWifiReconnectIntervalMs) {
            lastConnectAttempt = millis();
            Serial.println("WiFi: requesting automatic station reconnect");
            WiFi.reconnect();
        }

        if (connected) {
            if (otaStarted) ArduinoOTA.handle();
            if (otaInProgress) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            healthServer.handleClient();
            startGatewayPing();

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
        return true;
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

bool networkOtaInProgress() {
    return otaInProgress;
}

void networkConfirmOtaUiReady() {
    otaUiReady = true;
}
