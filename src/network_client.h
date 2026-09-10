#pragma once

#include <Arduino.h>

enum class RemoteCommand : uint8_t {
    Reset = 1,
    Blackout = 2,
    ShowHint = 3,
};

enum class SequenceReportState : uint8_t {
    Idle,
    Pending,
    InFlight,
    Acknowledged,
    Failed,
};

bool networkBegin(QueueHandle_t remoteCommandQueue);
bool networkSubmitSequence(const uint8_t *values, size_t count);
SequenceReportState networkSequenceReportState();
void networkAcknowledgeRemoteCommand(RemoteCommand command);
bool networkOtaInProgress();
void networkConfirmOtaUiReady();
