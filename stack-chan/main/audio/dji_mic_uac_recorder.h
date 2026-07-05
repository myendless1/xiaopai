#pragma once

#include <stddef.h>
#include <stdint.h>

struct DjiMicUacRecordStatus {
    bool started = false;
    bool wifi_connected = false;
    bool usb_connected = false;
    bool uploading = false;
    bool done = false;
    bool failed = false;
    uint32_t sample_rate = 0;
    uint16_t bit_resolution = 0;
    uint8_t channels = 0;
    uint32_t target_bytes = 0;
    uint32_t captured_bytes = 0;
    uint32_t bytes_sent = 0;
    uint32_t callback_bytes = 0;
    uint32_t dropped_bytes = 0;
    uint32_t callback_count = 0;
    uint32_t trace_count = 0;
    uint16_t first_frame_bit_resolution = 0;
    uint32_t first_frame_sample_rate = 0;
    uint32_t first_frame_bytes = 0;
    uint8_t first_frame_mod4 = 0;
    uint8_t first_frame_mod6 = 0;
    int http_status = 0;
    char url[128] = {};
    char detail[128] = {};
};

bool dji_mic_uac_recorder_start();
DjiMicUacRecordStatus dji_mic_uac_recorder_status();
