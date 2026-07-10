#pragma once

#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"

enum class AudioVadState {
    kUnknown = 0,
    kSilence,
    kSpeech,
};

enum class AudioInputSource {
    kInternalMic = 0,
    kDjiMicReceiver,
};

struct AudioInputStatus {
    AudioInputSource active_source = AudioInputSource::kInternalMic;
    bool dji_receiver_detected = false;
    bool dji_receiver_streaming = false;
    bool dji_receiver_capture_ready = false;
    bool dji_receiver_identity_confirmed = false;
    const char* dji_receiver_manufacturer = "";
    const char* dji_receiver_product = "";
    const char* detail = "";
};

const char* audio_input_source_name(AudioInputSource source);
const char* audio_input_source_label(AudioInputSource source);

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
AudioInputStatus audio_service_get_input_status();
void audio_service_abort_playback();
void audio_service_dump_state();
void audio_service_log_diagnostics();
void audio_service_register_heartbeat_task(TaskHandle_t handle);
void audio_service_register_wifi_debug_task(TaskHandle_t handle);
bool audio_service_test_tone(int sample_rate, int tone_hz, int duration_ms, int volume_percent);

bool audio_service_is_available();
bool audio_service_is_playing();
bool audio_service_wait_playback_idle(TickType_t timeout);
