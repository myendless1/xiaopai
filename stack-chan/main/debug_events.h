#pragma once

#include "freertos/FreeRTOS.h"

#include <stdint.h>

enum class DebugEventKind : uint8_t {
    LogLine = 0,
    StateChange = 1,
};

struct DebugEventItem {
    DebugEventKind kind = DebugEventKind::LogLine;
    uint32_t device_ms = 0;
    char source[24] = {};
    char from[32] = {};
    char to[32] = {};
    char reason[96] = {};
    char line[240] = {};
    uint32_t generation = 0;
    bool is_speaking = false;
    bool can_sample_mic = false;
    bool screen_visible = false;
    bool sleep_dark = false;
    bool animation_active = false;
};

bool debug_events_init();
bool debug_events_push_log_line(const char* line);
bool debug_events_push_xiaopai_state(const char* from, const char* to, uint32_t generation,
                                     bool is_speaking, bool can_sample_mic, const char* reason);
bool debug_events_push_expression_state(const char* from, const char* to, bool screen_visible,
                                        bool sleep_dark, bool animation_active, const char* reason);
bool debug_events_pop(DebugEventItem* out, TickType_t timeout_ticks);
