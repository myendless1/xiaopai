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
void show_expression_with_mouth(const char* expression, const char* mouth);
void show_idle_sleep_dark_if_due(uint32_t idle_ms);
void show_temporary_expression(const char* expression, uint32_t duration_ms);
void set_mouth_shape(const char* mouth);
void start_mouth_animation();
void stop_mouth_animation();
void update_mouth_from_speaking_level(uint8_t level);
void cancel_mouth_animation_now();
