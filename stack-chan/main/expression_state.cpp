#include "expression_state.h"

#include "esp_log.h"

#include <string.h>

namespace {

static constexpr const char* TAG = "ExpressionState";

void report_expression_change(const ExpressionStateSnapshot& before, const char* reason)
{
    ExpressionStateSnapshot after = expression_state_get();
    if (strcmp(before.name, after.name) == 0 &&
        before.screen_visible == after.screen_visible &&
        before.sleep_dark == after.sleep_dark &&
        before.animation_active == after.animation_active) {
        return;
    }
    ESP_LOGI(TAG,
             "EXPRESSION from=%s to=%s visible=%d sleep_dark=%d "
             "animation=%d reason=\"%s\"",
             before.name != nullptr ? before.name : "", after.name != nullptr ? after.name : "",
             after.screen_visible ? 1 : 0, after.sleep_dark ? 1 : 0, after.animation_active ? 1 : 0,
             reason != nullptr ? reason : "");
}

} // namespace

void expression_state_init(SemaphoreHandle_t m5_mutex, const ExpressionControllerHooks& hooks)
{
    expression_controller_init(m5_mutex, hooks);
}

ExpressionStateSnapshot expression_state_get()
{
    ExpressionControllerState state = expression_controller_current_state();
    ExpressionStateSnapshot snapshot;
    snapshot.name = state.name;
    snapshot.screen_visible = state.screen_visible;
    snapshot.sleep_dark = state.sleep_dark_visible;
    snapshot.animation_active = state.animation_active;
    return snapshot;
}

bool expression_state_set(const char* state)
{
    ExpressionStateSnapshot before = expression_state_get();
    show_expression(state);
    report_expression_change(before, state);
    return true;
}

void expression_state_set_temporary(const char* state, uint32_t duration_ms)
{
    ExpressionStateSnapshot before = expression_state_get();
    show_temporary_expression(state, duration_ms);
    report_expression_change(before, state);
}

void expression_state_mark_dirty()
{
    mark_expression_screen_dirty();
}

void expression_state_set_idle_sleep_if_due(uint32_t idle_ms)
{
    ExpressionStateSnapshot before = expression_state_get();
    show_idle_sleep_dark_if_due(idle_ms);
    report_expression_change(before, "idle sleep due");
}

void expression_state_start_speech_visual_feedback()
{
    ExpressionStateSnapshot before = expression_state_get();
    start_speech_visual_feedback();
    report_expression_change(before, "speech feedback start");
}

void expression_state_stop_speech_visual_feedback()
{
    ExpressionStateSnapshot before = expression_state_get();
    stop_speech_visual_feedback();
    report_expression_change(before, "speech feedback stop");
}
