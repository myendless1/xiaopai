#pragma once

#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"

enum class AudioVadState {
    kUnknown = 0,
    kSilence,
    kSpeech,
};

struct AudioPlayOptions {
    bool wait = false;
    bool drop_oldest = true;
};

bool audio_service_init();
bool audio_service_start();
void audio_service_stop();
void audio_service_set_volume_percent(int percent);
bool audio_service_play_pcm_16k(const int16_t* samples, size_t count,
                                AudioPlayOptions options = AudioPlayOptions{});
bool audio_service_play_opus_frame_16k(const uint8_t* data, size_t len);
size_t audio_service_read_clean_16k(int16_t* out, size_t samples, TickType_t timeout);
AudioVadState audio_service_get_vad_state();
void audio_service_abort_playback();
void audio_service_dump_state();
bool audio_service_test_tone(int sample_rate, int tone_hz, int duration_ms, int volume_percent);

bool audio_service_is_available();
bool audio_service_is_playing();
bool audio_service_wait_playback_idle(TickType_t timeout);
