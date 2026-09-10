#pragma once

#include "embedded_assets.h"

bool audioBegin();
void audioPlay(EmbeddedClip clip, uint8_t repeats = 1);
uint32_t audioPlayTracked(EmbeddedClip clip, uint8_t repeats = 1);
bool audioPlaybackComplete(uint32_t playbackId);
void audioStop();
bool audioSetVolume(uint8_t volume);
uint8_t audioGetVolume();
