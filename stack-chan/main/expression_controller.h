#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdint.h>

extern const char* const kDefaultExpression;
static constexpr uint32_t kHeadTouchShyExpressionMs = 5000;

struct ExpressionControllerHooks {
    void (*start_speaking_light_animation)();
    void (*stop_speaking_light_animation)();
    void (*set_light_strip_listening)();
    void (*set_light_strip_sleeping)();
    bool (*should_restore_listening_light)();
};

void expression_controller_init(SemaphoreHandle_t m5_mutex, const ExpressionControllerHooks& hooks);
void mark_expression_screen_dirty();
bool expression_screen_is_visible();
bool speaking_animation_is_running();
void expression_set_speech_overridden(bool overridden);
void show_expression(const char* expression);
void show_temporary_expression(const char* expression, uint32_t duration_ms);
void start_speaking_animation();
void stop_speaking_animation();
void cancel_speaking_animation_now();
