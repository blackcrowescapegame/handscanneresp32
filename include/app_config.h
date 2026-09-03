#pragma once

// Put private values in include/secrets.h. That file is intentionally ignored.
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef HANDSCANNER_WIFI_SSID
#define HANDSCANNER_WIFI_SSID ""
#endif

#ifndef HANDSCANNER_WIFI_PASSWORD
#define HANDSCANNER_WIFI_PASSWORD ""
#endif

#ifndef HANDSCANNER_HA_EVENT_URL
#define HANDSCANNER_HA_EVENT_URL "http://192.168.70.113:8123/api/events/bc_custom_event"
#endif

#ifndef HANDSCANNER_RESET_URL
#define HANDSCANNER_RESET_URL "http://192.168.70.113:1880/endpoint/resetHandScanner"
#endif

#ifndef HANDSCANNER_BEARER_TOKEN
#define HANDSCANNER_BEARER_TOKEN ""
#endif

#ifndef HANDSCANNER_RESET_POLL_MS
#define HANDSCANNER_RESET_POLL_MS 3000UL
#endif

#ifndef HANDSCANNER_HTTP_TIMEOUT_MS
#define HANDSCANNER_HTTP_TIMEOUT_MS 2500U
#endif

#ifndef HANDSCANNER_AUDIO_VOLUME
#define HANDSCANNER_AUDIO_VOLUME 90
#endif

#ifndef HANDSCANNER_OTA_HOSTNAME
#define HANDSCANNER_OTA_HOSTNAME "handscanner"
#endif

// OTA remains disabled until a non-empty password is defined in secrets.h.
#ifndef HANDSCANNER_OTA_PASSWORD
#define HANDSCANNER_OTA_PASSWORD ""
#endif

#ifndef HANDSCANNER_OTA_PORT
#define HANDSCANNER_OTA_PORT 3232
#endif

// Native portrait orientation for the 800x1280 panel.
#define HANDSCANNER_DISPLAY_ROTATION 0
