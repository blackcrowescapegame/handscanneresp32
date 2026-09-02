#pragma once

#include <stddef.h>
#include <stdint.h>

extern const uint8_t base_start[] asm("_binary_data_ui_base_rgb565_start");
extern const uint8_t base_end[] asm("_binary_data_ui_base_rgb565_end");
extern const uint8_t skull_start[] asm("_binary_data_ui_skull_rgb565_start");
extern const uint8_t skull_end[] asm("_binary_data_ui_skull_rgb565_end");
extern const uint8_t hint_start[] asm("_binary_data_ui_hint_rgb565_start");
extern const uint8_t hint_end[] asm("_binary_data_ui_hint_rgb565_end");

extern const uint8_t finger_start[] asm("_binary_data_audio_finger_pcm_start");
extern const uint8_t finger_end[] asm("_binary_data_audio_finger_pcm_end");
extern const uint8_t granted_start[] asm("_binary_data_audio_granted_pcm_start");
extern const uint8_t granted_end[] asm("_binary_data_audio_granted_pcm_end");
extern const uint8_t denied_start[] asm("_binary_data_audio_denied_pcm_start");
extern const uint8_t denied_end[] asm("_binary_data_audio_denied_pcm_end");
extern const uint8_t riser_start[] asm("_binary_data_audio_riser_pcm_start");
extern const uint8_t riser_end[] asm("_binary_data_audio_riser_pcm_end");

struct EmbeddedClip {
    const uint8_t *data;
    size_t length;
};

inline EmbeddedClip fingerClip() { return {finger_start, static_cast<size_t>(finger_end - finger_start)}; }
inline EmbeddedClip grantedClip() { return {granted_start, static_cast<size_t>(granted_end - granted_start)}; }
inline EmbeddedClip deniedClip() { return {denied_start, static_cast<size_t>(denied_end - denied_start)}; }
inline EmbeddedClip riserClip() { return {riser_start, static_cast<size_t>(riser_end - riser_start)}; }

