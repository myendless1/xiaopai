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
    bool identity_confirmed = false;
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    int sample_rate = 0;
    int channels = 0;
    const char* speed = "";
    const char* manufacturer = "";
    const char* product = "";
    const char* detail = "";
};

bool dji_mic_receiver_input_start();
DjiMicReceiverStatus dji_mic_receiver_input_status();
size_t dji_mic_receiver_input_read_16k(int16_t* out, size_t samples, TickType_t timeout);
