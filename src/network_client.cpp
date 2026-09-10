#include "network_client.h"

#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <ping/ping_sock.h>
#include <time.h>

#include "app_config.h"
#include "audio_engine.h"

namespace {
constexpr uint32_t kWifiReconnectIntervalMs = 10000;
constexpr uint32_t kGatewayPingIntervalMs = 10000;
constexpr uint32_t kGatewayPingTimeoutMs = 1000;
constexpr uint8_t kFailedProbesBeforeRadioReset = 3;
constexpr uint32_t kOtaUiReadyTimeoutMs = 3000;
constexpr uint32_t kRadioRecoveryWindowMs = 120000;
constexpr uint8_t kRadioRecoveriesBeforeRestart = 2;
constexpr uint32_t kPersistedSequenceMagic = 0x48534E31;
constexpr char kCommandIdHeader[] = "X-Handscanner-Command-Id";

struct SequenceMessage {
    uint8_t values[5];
    uint8_t count;
};

struct PersistedSequence {
    uint32_t magic;
    uint8_t values[5];
    uint8_t count;
};

QueueHandle_t sequenceQueue = nullptr;
QueueHandle_t commandQueue = nullptr;
QueueHandle_t commandAcknowledgementQueue = nullptr;

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
uint8_t recentRadioRecoveries = 0;
uint32_t radioRecoveryWindowStartedAt = 0;
uint32_t wifiDisconnectedSince = 0;
bool rebootRequested = false;
uint32_t rebootRequestedAt = 0;
String pendingRestartReason = "none";
String lastRecoveryReason = "none";
uint32_t recoveryRestartCount = 0;
uint32_t unexpectedResetCount = 0;
uint32_t bootCount = 0;
esp_reset_reason_t bootResetReason = ESP_RST_UNKNOWN;
bool networkWatchdogEnabled = false;
bool httpRequestInFlight = false;
String httpRequestKind = "none";
uint32_t httpRequestStartedAt = 0;
uint32_t httpFailureCount = 0;
uint8_t consecutiveHttpFailures = 0;
uint32_t httpGuardRejectCount = 0;
int lastHttpStatus = 0;
String lastHttpOperation = "none";
volatile SequenceReportState sequenceReportState = SequenceReportState::Idle;
String activeHintCommandId;
Preferences preferences;
bool preferencesReady = false;
esp_ping_handle_t gatewayPing = nullptr;
WebServer healthServer(HANDSCANNER_HEALTH_API_PORT);

const char *resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "power_on";
        case ESP_RST_EXT: return "external";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt_watchdog";
        case ESP_RST_TASK_WDT: return "task_watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        default: return "unknown";
    }
}

bool isUnexpectedReset(esp_reset_reason_t reason) {
    return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
           reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT ||
           reason == ESP_RST_BROWNOUT;
}

const char *reportStateName(SequenceReportState state) {
    switch (state) {
        case SequenceReportState::Idle: return "idle";
        case SequenceReportState::Pending: return "pending";
        case SequenceReportState::InFlight: return "in_flight";
        case SequenceReportState::Acknowledged: return "acknowledged";
        case SequenceReportState::Failed: return "failed";
        default: return "unknown";
    }
}

void initializeDiagnostics() {
    bootResetReason = esp_reset_reason();
    preferencesReady = preferences.begin("handscanner", false);
    if (!preferencesReady) {
        Serial.println("Diagnostics: NVS unavailable");
        return;
    }

    bootCount = preferences.getUInt("bootCount", 0) + 1;
    preferences.putUInt("bootCount", bootCount);
    recoveryRestartCount = preferences.getUInt("recoveries", 0);
    unexpectedResetCount = preferences.getUInt("crashes", 0);
    if (isUnexpectedReset(bootResetReason)) {
        ++unexpectedResetCount;
        preferences.putUInt("crashes", unexpectedResetCount);
    }
    lastRecoveryReason = preferences.getString("restartWhy", "none");
}

void configureNetworkWatchdog() {
    const esp_task_wdt_config_t config = {
        .timeout_ms = HANDSCANNER_NETWORK_WATCHDOG_MS,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_err_t result = esp_task_wdt_reconfigure(&config);
    if (result == ESP_ERR_INVALID_STATE) result = esp_task_wdt_init(&config);
    if (result == ESP_OK) result = esp_task_wdt_add(nullptr);
    networkWatchdogEnabled = result == ESP_OK || result == ESP_ERR_INVALID_STATE;
    Serial.printf("Network watchdog: %s (%d)\n", networkWatchdogEnabled ? "enabled" : "unavailable",
                  static_cast<int>(result));
}

void scheduleRecoveryRestart(const char *reason) {
    if (rebootRequested || otaInProgress) return;
    pendingRestartReason = reason;
    lastRecoveryReason = reason;
    ++recoveryRestartCount;
    if (preferencesReady) {
        preferences.putString("restartWhy", lastRecoveryReason);
        preferences.putUInt("recoveries", recoveryRestartCount);
    }
    rebootRequestedAt = millis();
    rebootRequested = true;
    Serial.printf("Recovery: restart scheduled (%s)\n", reason);
}

bool beginHttpRequest(const char *operation) {
    if (httpRequestInFlight) {
        ++httpGuardRejectCount;
        Serial.printf("API: blocked overlapping %s while %s is in flight\n", operation,
                      httpRequestKind.c_str());
        return false;
    }
    httpRequestInFlight = true;
    httpRequestKind = operation;
    httpRequestStartedAt = millis();
    lastHttpOperation = operation;
    return true;
}

void finishHttpRequest(int status, bool critical, const char *failureReason) {
    lastHttpStatus = status;
    httpRequestInFlight = false;
    httpRequestKind = "none";
    httpRequestStartedAt = 0;
    if (status >= 200 && status < 300) {
        consecutiveHttpFailures = 0;
        return;
    }

    ++httpFailureCount;
    if (consecutiveHttpFailures < UINT8_MAX) ++consecutiveHttpFailures;
    if (critical || consecutiveHttpFailures >= HANDSCANNER_HTTP_FAILURES_BEFORE_RESTART) {
        scheduleRecoveryRestart(failureReason);
    }
}

void sendJson(int status, const String &body) {
    healthServer.sendHeader("Cache-Control", "no-store");
    healthServer.sendHeader("Access-Control-Allow-Origin", "*");
    healthServer.send(status, "application/json", body);
}

void handleVolumeRequest(const String &value) {
    if (value.isEmpty() || value.length() > 3) {
        sendJson(400, "{\"status\":\"error\",\"error\":\"volume must be between 0 and 100\"}");
        return;
    }
    for (size_t i = 0; i < value.length(); ++i) {
        if (!isDigit(value[i])) {
            sendJson(400, "{\"status\":\"error\",\"error\":\"volume must be between 0 and 100\"}");
            return;
        }
    }

    const long volume = value.toInt();
    if (volume < 0 || volume > 100) {
        sendJson(400, "{\"status\":\"error\",\"error\":\"volume must be between 0 and 100\"}");
        return;
    }

    if (!audioSetVolume(static_cast<uint8_t>(volume))) {
        sendJson(503, "{\"status\":\"error\",\"error\":\"audio codec unavailable\"}");
        return;
    }

    sendJson(200, String("{\"status\":\"ok\",\"volume\":") + audioGetVolume() + "}");
}

void handleUnknownRequest() {
    constexpr char kVolumePrefix[] = "/api/volume/";
    const String uri = healthServer.uri();
    if ((healthServer.method() == HTTP_GET || healthServer.method() == HTTP_POST) &&
        uri.startsWith(kVolumePrefix)) {
        handleVolumeRequest(uri.substring(strlen(kVolumePrefix)));
        return;
    }

    sendJson(404, "{\"status\":\"error\",\"error\":\"not found\"}");
}

void handleRebootRequest() {
    if (otaInProgress) {
        sendJson(409, "{\"status\":\"error\",\"error\":\"OTA update in progress\"}");
        return;
    }

    pendingRestartReason = "api_requested";
    rebootRequestedAt = millis();
    rebootRequested = true;
    sendJson(202, "{\"status\":\"ok\",\"state\":\"rebooting\"}");
}

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

    IPAddress localIp;
    IPAddress gateway;
    IPAddress subnet;
    IPAddress dns;
    if (!localIp.fromString(HANDSCANNER_WIFI_LOCAL_IP) ||
        !gateway.fromString(HANDSCANNER_WIFI_GATEWAY) ||
        !subnet.fromString(HANDSCANNER_WIFI_SUBNET) ||
        !dns.fromString(HANDSCANNER_WIFI_DNS)) {
        Serial.println("WiFi: invalid static IPv4 configuration");
        return;
    }
    if (!WiFi.config(localIp, gateway, subnet, dns)) {
        Serial.println("WiFi: failed to apply static IPv4 configuration");
        return;
    }
    Serial.printf("WiFi: static IP %s, gateway %s, subnet %s, DNS %s\n",
                  localIp.toString().c_str(), gateway.toString().c_str(),
                  subnet.toString().c_str(), dns.toString().c_str());
}

void beginStationConnection() {
    Serial.printf("WiFi: connecting to %s\n", HANDSCANNER_WIFI_SSID);
    WiFi.begin(HANDSCANNER_WIFI_SSID, HANDSCANNER_WIFI_PASSWORD);
}

void reconnectWifiRadio() {
    if (otaInProgress) return;

    radioReconnectRequested = false;
    const uint32_t now = millis();
    if (radioRecoveryWindowStartedAt == 0 || now - radioRecoveryWindowStartedAt > kRadioRecoveryWindowMs) {
        radioRecoveryWindowStartedAt = now;
        recentRadioRecoveries = 0;
    }
    if (++recentRadioRecoveries >= kRadioRecoveriesBeforeRestart) {
        scheduleRecoveryRestart("wifi_stack_unresponsive");
        return;
    }
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
            body.reserve(1100);
            body = "{\"status\":\"ok\",\"state\":\"";
            body += otaInProgress ? "ota_updating" : "ready";
            body += "\",\"version\":\"" HANDSCANNER_FIRMWARE_VERSION "\"";
            body += ",\"uptime_ms\":" + String(millis());
            body += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
            body += ",\"rssi_dbm\":" + String(WiFi.RSSI());
            body += ",\"gateway\":\"" + WiFi.gatewayIP().toString() + "\"";
            body += ",\"gateway_probe_failures\":" + String(failedGatewayProbes);
            body += ",\"wifi_radio_reconnects\":" + String(radioReconnectCount);
            body += ",\"wifi_recent_recoveries\":" + String(recentRadioRecoveries);
            body += ",\"wifi_sleep_enabled\":false";
            body += ",\"free_heap_bytes\":" + String(ESP.getFreeHeap());
            body += ",\"min_free_heap_bytes\":" + String(ESP.getMinFreeHeap());
            body += ",\"largest_free_heap_block_bytes\":" +
                    String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            body += ",\"psram_size_bytes\":" + String(ESP.getPsramSize());
            body += ",\"free_psram_bytes\":" + String(ESP.getFreePsram());
            body += ",\"min_free_psram_bytes\":" + String(ESP.getMinFreePsram());
            body += ",\"reset_reason\":\"" + String(resetReasonName(bootResetReason)) + "\"";
            body += ",\"reset_reason_code\":" + String(static_cast<int>(bootResetReason));
            body += ",\"boot_count\":" + String(bootCount);
            body += ",\"unexpected_reset_count\":" + String(unexpectedResetCount);
            body += ",\"recovery_restart_count\":" + String(recoveryRestartCount);
            body += ",\"last_recovery_reason\":\"" + lastRecoveryReason + "\"";
            body += ",\"restart_pending\":";
            body += rebootRequested ? "true" : "false";
            body += ",\"restart_reason\":\"" + pendingRestartReason + "\"";
            body += ",\"network_watchdog_enabled\":";
            body += networkWatchdogEnabled ? "true" : "false";
            body += ",\"http_in_flight\":";
            body += httpRequestInFlight ? "true" : "false";
            body += ",\"http_operation\":\"" + httpRequestKind + "\"";
            body += ",\"http_in_flight_ms\":" +
                    String(httpRequestInFlight ? millis() - httpRequestStartedAt : 0);
            body += ",\"http_last_status\":" + String(lastHttpStatus);
            body += ",\"http_last_operation\":\"" + lastHttpOperation + "\"";
            body += ",\"http_failures_total\":" + String(httpFailureCount);
            body += ",\"http_failures_consecutive\":" + String(consecutiveHttpFailures);
            body += ",\"http_guard_rejections\":" + String(httpGuardRejectCount);
            body += ",\"ha_report_state\":\"" +
                    String(reportStateName(sequenceReportState)) + "\"";
            body += ",\"volume\":" + String(audioGetVolume());
            body += ",\"ota_enabled\":";
            body += strlen(HANDSCANNER_OTA_PASSWORD) > 0 ? "true" : "false";
            body += ",\"ota_state\":\"" + String(otaState) + "\"";
            body += ",\"ota_error\":" + String(lastOtaError);
            body += ",\"ota_update_error\":" + String(lastUpdateError);
            body += ",\"ota_update_error_message\":\"" + lastUpdateErrorMessage + "\"";
            body += ",\"ota_partition\":\"" + String(partition) + "\"}";

            sendJson(200, body);
        });
        healthServer.on("/api/reboot", HTTP_GET, handleRebootRequest);
        healthServer.on("/api/reboot", HTTP_POST, handleRebootRequest);
        healthServer.onNotFound(handleUnknownRequest);
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
    Serial.printf("OTA: ready at %s:%u (%s.local)\n", WiFi.localIP().toString().c_str(),
                  static_cast<unsigned int>(HANDSCANNER_OTA_PORT), HANDSCANNER_OTA_HOSTNAME);
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

bool sendSequence(const SequenceMessage &message) {
    if (strlen(HANDSCANNER_HA_EVENT_URL) == 0) {
        sequenceReportState = SequenceReportState::Acknowledged;
        return true;
    }
    if (!beginHttpRequest("home_assistant_event")) return false;

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
        finishHttpRequest(HTTPC_ERROR_CONNECTION_REFUSED, true, "ha_request_failed");
        sequenceReportState = SequenceReportState::Failed;
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    if (strlen(HANDSCANNER_BEARER_TOKEN) > 0) {
        http.addHeader("Authorization", String("Bearer ") + HANDSCANNER_BEARER_TOKEN);
    }
    sequenceReportState = SequenceReportState::InFlight;
    const int status = http.POST(payload);
    Serial.printf("API: Home Assistant POST -> %d\n", status);
    http.end();

    const bool acknowledged = status >= 200 && status < 300;
    finishHttpRequest(status, !acknowledged, "ha_request_failed");
    if (!acknowledged) {
        sequenceReportState = SequenceReportState::Failed;
        return false;
    }

    lastUserInput = currentState;
    lastChanged = changed;
    lastReported = reported;
    lastUpdated = updated;
    sequenceReportState = SequenceReportState::Acknowledged;
    return true;
}

void pollRemoteCommand() {
    if (strlen(HANDSCANNER_RESET_URL) == 0) return;
    if (!beginHttpRequest("remote_command_poll")) return;

    HTTPClient http;
    http.setConnectTimeout(HANDSCANNER_HTTP_TIMEOUT_MS);
    http.setTimeout(HANDSCANNER_HTTP_TIMEOUT_MS);
    if (!http.begin(HANDSCANNER_RESET_URL)) {
        finishHttpRequest(HTTPC_ERROR_CONNECTION_REFUSED, false, "remote_poll_failed");
        return;
    }
    const char *responseHeaders[] = {kCommandIdHeader};
    http.collectHeaders(responseHeaders, 1);
    const int status = http.GET();
    if (status >= 200 && status < 300) {
        String body = http.getString();
        body.trim();
        const int value = body.toInt();
        if (value >= 1 && value <= 3) {
            const String commandId = http.header(kCommandIdHeader);
            const bool duplicatePendingHint = value == static_cast<int>(RemoteCommand::ShowHint) &&
                                              !commandId.isEmpty() &&
                                              commandId == activeHintCommandId;
            if (!duplicatePendingHint) {
                const RemoteCommand command = static_cast<RemoteCommand>(value);
                if (xQueueSend(commandQueue, &command, 0) == pdTRUE) {
                    if (command == RemoteCommand::ShowHint) activeHintCommandId = commandId;
                    Serial.printf("API: remote command %d%s%s\n", value,
                                  commandId.isEmpty() ? "" : " id=",
                                  commandId.c_str());
                }
            }
        }
    } else {
        Serial.printf("API: reset poll failed (%d)\n", status);
    }
    http.end();
    finishHttpRequest(status, false, "remote_poll_failed");
}

void acknowledgeHint() {
    if (activeHintCommandId.isEmpty() || strlen(HANDSCANNER_HINT_ACK_URL) == 0) return;
    if (!beginHttpRequest("hint_acknowledgement")) return;

    HTTPClient http;
    http.setConnectTimeout(HANDSCANNER_HTTP_TIMEOUT_MS);
    http.setTimeout(HANDSCANNER_HTTP_TIMEOUT_MS);
    if (!http.begin(HANDSCANNER_HINT_ACK_URL)) {
        finishHttpRequest(HTTPC_ERROR_CONNECTION_REFUSED, true, "hint_ack_failed");
        return;
    }
    http.addHeader("Content-Type", "application/json");
    const String payload = String("{\"command\":3,\"commandId\":\"") +
                           activeHintCommandId + "\"}";
    const int status = http.POST(payload);
    Serial.printf("API: hint acknowledgement -> %d (id=%s)\n", status,
                  activeHintCommandId.c_str());
    http.end();
    const bool acknowledged = status >= 200 && status < 300;
    finishHttpRequest(status, !acknowledged, "hint_ack_failed");
    if (acknowledged) activeHintCommandId = "";
}

bool loadPersistedSequence(SequenceMessage &message) {
    if (!preferencesReady || preferences.getBytesLength("pendingSeq") != sizeof(PersistedSequence)) {
        return false;
    }
    PersistedSequence stored{};
    if (preferences.getBytes("pendingSeq", &stored, sizeof(stored)) != sizeof(stored) ||
        stored.magic != kPersistedSequenceMagic || stored.count == 0 ||
        stored.count > sizeof(stored.values)) {
        preferences.remove("pendingSeq");
        return false;
    }
    memcpy(message.values, stored.values, sizeof(message.values));
    message.count = stored.count;
    return true;
}

void persistSequence(const SequenceMessage &message) {
    if (!preferencesReady) return;
    PersistedSequence stored{};
    stored.magic = kPersistedSequenceMagic;
    memcpy(stored.values, message.values, sizeof(stored.values));
    stored.count = message.count;
    preferences.putBytes("pendingSeq", &stored, sizeof(stored));
}

void clearPersistedSequence() {
    if (preferencesReady) preferences.remove("pendingSeq");
}

void networkTask(void *) {
    uint32_t lastConnectAttempt = millis();
    uint32_t lastPoll = millis();
    SequenceMessage pendingMessage{};
    bool hasPendingMessage = false;

    initializeDiagnostics();
    configureNetworkWatchdog();
    hasPendingMessage = loadPersistedSequence(pendingMessage);
    if (hasPendingMessage) {
        sequenceReportState = SequenceReportState::Pending;
        Serial.println("API: restored an unacknowledged Home Assistant event");
    }
    WiFi.onEvent(wifiEvent);
    configureStation();
    beginStationConnection();
    wifiDisconnectedSince = millis();

    for (;;) {
        if (networkWatchdogEnabled) esp_task_wdt_reset();
        if (!hasPendingMessage && xQueueReceive(sequenceQueue, &pendingMessage, 0) == pdTRUE) {
            hasPendingMessage = true;
            sequenceReportState = SequenceReportState::Pending;
            persistSequence(pendingMessage);
        }

        bool connected = WiFi.status() == WL_CONNECTED;
        if (connected != wifiWasConnected) {
            wifiWasConnected = connected;
            if (connected) {
                wifiDisconnectedSince = 0;
                configTime(0, 0, "pool.ntp.org", "time.nist.gov");
                startHealthApi();
                startOta();
                startGatewayPing();
            } else {
                if (wifiDisconnectedSince == 0) wifiDisconnectedSince = millis();
                stopNetworkServices();
            }
        }

        if (!connected && wifiDisconnectedSince != 0 &&
            millis() - wifiDisconnectedSince >= HANDSCANNER_WIFI_DISCONNECTED_RESTART_MS) {
            scheduleRecoveryRestart("wifi_disconnected_timeout");
        }

        if (rebootRequested && !otaInProgress && millis() - rebootRequestedAt >= 500) {
            Serial.printf("Recovery: restarting device (%s)\n", pendingRestartReason.c_str());
            Serial.flush();
            ESP.restart();
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

            RemoteCommand acknowledgedCommand;
            while (xQueueReceive(commandAcknowledgementQueue, &acknowledgedCommand, 0) == pdTRUE) {
                if (acknowledgedCommand == RemoteCommand::ShowHint) acknowledgeHint();
            }
            if (hasPendingMessage && !rebootRequested) {
                if (sendSequence(pendingMessage)) {
                    hasPendingMessage = false;
                    clearPersistedSequence();
                }
            }
            if (!rebootRequested && millis() - lastPoll >= HANDSCANNER_RESET_POLL_MS) {
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
    commandAcknowledgementQueue = xQueueCreate(4, sizeof(RemoteCommand));
    if (commandQueue == nullptr || sequenceQueue == nullptr ||
        commandAcknowledgementQueue == nullptr) return false;
    if (strlen(HANDSCANNER_WIFI_SSID) == 0) {
        Serial.println("WiFi: disabled; copy include/secrets.example.h to include/secrets.h and configure it");
        return true;
    }
    return xTaskCreate(networkTask, "handscanner-network", 8192, nullptr, 1, nullptr) == pdPASS;
}

bool networkSubmitSequence(const uint8_t *values, size_t count) {
    if (sequenceQueue == nullptr || values == nullptr || count == 0) return false;
    SequenceMessage message{};
    message.count = min(count, sizeof(message.values));
    memcpy(message.values, values, message.count);
    sequenceReportState = SequenceReportState::Pending;
    return xQueueSend(sequenceQueue, &message, 0) == pdTRUE;
}

SequenceReportState networkSequenceReportState() {
    return sequenceReportState;
}

void networkAcknowledgeRemoteCommand(RemoteCommand command) {
    if (commandAcknowledgementQueue == nullptr) return;
    xQueueSend(commandAcknowledgementQueue, &command, 0);
}

bool networkOtaInProgress() {
    return otaInProgress;
}

void networkConfirmOtaUiReady() {
    otaUiReady = true;
}
