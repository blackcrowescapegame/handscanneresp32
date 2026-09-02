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

// Landscape rotation. 1 places the long edge horizontally with the board's
// native top on the left; use 3 and update transformTouch() if mounted opposite.
#define HANDSCANNER_DISPLAY_ROTATION 1

