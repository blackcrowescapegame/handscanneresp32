#pragma once

#include <Arduino.h>

enum class RemoteCommand : uint8_t {
    Reset = 1,
    Blackout = 2,
    ShowHint = 3,
};

bool networkBegin(QueueHandle_t remoteCommandQueue);
void networkSubmitSequence(const uint8_t *values, size_t count);

