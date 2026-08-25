#pragma once

#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"

struct DjiMicReceiverStatus {
    bool detected = false;
    bool target_vid_pid = false;
    bool full_speed = false;
    bool audio_control = false;
    bool audio_streaming = false;
    bool capture_ready = false;
    bool stream_stable = false;
    bool identity_confirmed = false;
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    int sample_rate = 0;
    int channels = 0;
    int bit_resolution = 0;
    int selected_channel = 0;
    uint32_t connection_generation = 0;
    uint32_t callback_count = 0;
    uint32_t callback_bytes = 0;
    uint32_t output_samples = 0;
    uint32_t raw_drops = 0;
    uint32_t pcm_drops = 0;
    uint32_t format_errors = 0;
    const char* speed = "";
    const char* manufacturer = "";
    const char* product = "";
    const char* detail = "";
};

bool dji_mic_receiver_input_start();
void dji_mic_receiver_input_stop(const char* reason = nullptr);
bool dji_mic_receiver_input_is_enabled();
DjiMicReceiverStatus dji_mic_receiver_input_status();
size_t dji_mic_receiver_input_read_16k(int16_t* out, size_t samples, TickType_t timeout);
