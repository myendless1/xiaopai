#pragma once

#include "expression_controller.h"

#include <stdint.h>

struct ExpressionStateSnapshot {
    const char* name = "";
    bool screen_visible = false;
    bool sleep_dark = false;
    bool animation_active = false;
};

void expression_state_init(SemaphoreHandle_t m5_mutex, const ExpressionControllerHooks& hooks);
ExpressionStateSnapshot expression_state_get();
bool expression_state_set(const char* state);
void expression_state_set_temporary(const char* state, uint32_t duration_ms);
void expression_state_mark_dirty();
void expression_state_set_idle_sleep_if_due(uint32_t idle_ms);
void expression_state_start_speech_visual_feedback();
void expression_state_stop_speech_visual_feedback();
