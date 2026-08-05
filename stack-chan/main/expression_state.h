#pragma once

#include "expression_controller.h"

#include <stdint.h>

struct ExpressionStateSnapshot {
    const char* name = "";
    bool screen_visible = false;
    bool sleep_dark = false;
    bool animation_active = false;
};

enum class ReplyExpressionPhase : uint8_t {
    None,
    PendingAudio,
    Playing,
    BetweenSegments,
    Releasing,
};

struct ReplyExpressionSession {
    char turn_id[40] = {};
    uint32_t generation = 0;
    char expression[16] = "calm";
    ReplyExpressionPhase phase = ReplyExpressionPhase::None;
    uint32_t last_audio_ms = 0;
};

void expression_state_init(SemaphoreHandle_t m5_mutex, const ExpressionControllerHooks& hooks);
ExpressionStateSnapshot expression_state_get();
bool expression_state_set(const char* state);
void expression_state_set_temporary(const char* state, uint32_t duration_ms);
void expression_state_mark_dirty();
void expression_state_set_idle_sleep_if_due(uint32_t idle_ms);
void expression_state_start_speech_visual_feedback();
void expression_state_stop_speech_visual_feedback();
ReplyExpressionSession expression_state_reply_session();
void expression_state_reply_prepare(const char* turn_id, uint32_t generation, const char* expression);
void expression_state_reply_audio_started(const char* turn_id, uint32_t generation);
void expression_state_reply_segment_finished(const char* turn_id, uint32_t generation);
void expression_state_reply_end(const char* turn_id, uint32_t generation, bool cancelled);
void expression_state_reply_failed(const char* turn_id, uint32_t generation);
void expression_state_reply_cancel(const char* reason);
