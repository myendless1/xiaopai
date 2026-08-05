#include "expression_state.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

namespace {

static constexpr const char* TAG = "ExpressionState";
static constexpr uint32_t kReplyReleaseMs = 200;
static constexpr uint32_t kReplyWatchdogMs = 20000;

SemaphoreHandle_t reply_mutex = nullptr;
TaskHandle_t reply_watchdog_task = nullptr;
ReplyExpressionSession reply_session = {};
uint32_t reply_release_at_ms = 0;

uint32_t now_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

bool reply_matches_locked(const char* turn_id, uint32_t generation)
{
    return reply_session.phase != ReplyExpressionPhase::None &&
           reply_session.generation == generation &&
           strcmp(reply_session.turn_id, turn_id != nullptr ? turn_id : "") == 0;
}

bool reply_active()
{
    if (reply_mutex == nullptr) {
        return false;
    }
    xSemaphoreTake(reply_mutex, portMAX_DELAY);
    const bool active = reply_session.phase != ReplyExpressionPhase::None;
    xSemaphoreGive(reply_mutex);
    return active;
}

void clear_reply_locked()
{
    reply_session = {};
    snprintf(reply_session.expression, sizeof(reply_session.expression), "%s", kDefaultExpression);
    reply_release_at_ms = 0;
}

bool is_default_expression(const char* state)
{
    const char* value = state != nullptr ? state : "";
    return value[0] == '\0' || strcmp(value, "calm") == 0 || strcmp(value, "default") == 0 ||
           strcmp(value, "listening") == 0 || strcmp(value, "wake") == 0 || strcmp(value, "awake") == 0;
}

bool is_forced_expression(const char* state)
{
    const char* value = state != nullptr ? state : "";
    return strcmp(value, "sleep") == 0 || strcmp(value, "sleeping") == 0 ||
           strcmp(value, "sleep_dark") == 0 || strcmp(value, "stopped") == 0 ||
           strcmp(value, "screen_off") == 0;
}

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
    if (reply_mutex == nullptr) {
        reply_mutex = xSemaphoreCreateMutex();
    }
    if (reply_watchdog_task == nullptr) {
        xTaskCreatePinnedToCore([](void*) {
            while (true) {
                vTaskDelay(pdMS_TO_TICKS(100));
                bool restore_calm = false;
                bool watchdog = false;
                char turn_id[40] = {};
                if (reply_mutex == nullptr) {
                    continue;
                }
                xSemaphoreTake(reply_mutex, portMAX_DELAY);
                const uint32_t now = now_ms();
                if (reply_session.phase == ReplyExpressionPhase::Releasing &&
                    static_cast<int32_t>(now - reply_release_at_ms) >= 0) {
                    snprintf(turn_id, sizeof(turn_id), "%s", reply_session.turn_id);
                    clear_reply_locked();
                    restore_calm = true;
                } else if ((reply_session.phase == ReplyExpressionPhase::BetweenSegments ||
                            reply_session.phase == ReplyExpressionPhase::PendingAudio) &&
                           static_cast<uint32_t>(now - reply_session.last_audio_ms) >= kReplyWatchdogMs) {
                    snprintf(turn_id, sizeof(turn_id), "%s", reply_session.turn_id);
                    clear_reply_locked();
                    restore_calm = true;
                    watchdog = true;
                }
                xSemaphoreGive(reply_mutex);
                if (restore_calm) {
                    show_expression(kDefaultExpression);
                    if (watchdog) {
                        ESP_LOGW(TAG, "Reply expression watchdog restored calm: turn=%s", turn_id);
                    }
                }
            }
        }, "reply_expr", 3 * 1024, nullptr, 2, &reply_watchdog_task, 0);
    }
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
    if (reply_active()) {
        if (is_default_expression(state)) {
            return true;
        }
        expression_state_reply_cancel(is_forced_expression(state) ? "forced system expression" : "expression override");
    }
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

ReplyExpressionSession expression_state_reply_session()
{
    ReplyExpressionSession snapshot = {};
    if (reply_mutex == nullptr) {
        return snapshot;
    }
    xSemaphoreTake(reply_mutex, portMAX_DELAY);
    snapshot = reply_session;
    xSemaphoreGive(reply_mutex);
    return snapshot;
}

void expression_state_reply_prepare(const char* turn_id, uint32_t generation, const char* expression)
{
    if (reply_mutex == nullptr) {
        return;
    }
    const char* id = turn_id != nullptr ? turn_id : "";
    const char* requested = expression != nullptr ? expression : "";
    const char* normalized = strcmp(requested, "happy") == 0 || strcmp(requested, "thinking") == 0 ||
                                     strcmp(requested, "surprised") == 0
                                 ? requested
                                 : kDefaultExpression;
    bool new_session = false;
    xSemaphoreTake(reply_mutex, portMAX_DELAY);
    if (!reply_matches_locked(id, generation)) {
        clear_reply_locked();
        snprintf(reply_session.turn_id, sizeof(reply_session.turn_id), "%s", id);
        reply_session.generation = generation;
        snprintf(reply_session.expression, sizeof(reply_session.expression), "%s", normalized);
        reply_session.phase = ReplyExpressionPhase::PendingAudio;
        new_session = true;
    }
    reply_session.last_audio_ms = now_ms();
    xSemaphoreGive(reply_mutex);
    if (new_session) {
        show_expression(kDefaultExpression);
        ESP_LOGI(TAG, "Reply expression pending: turn=%s generation=%u expression=%s",
                 id, static_cast<unsigned>(generation), normalized);
    }
}

void expression_state_reply_audio_started(const char* turn_id, uint32_t generation)
{
    char expression[16] = {};
    bool show = false;
    if (reply_mutex == nullptr) {
        return;
    }
    xSemaphoreTake(reply_mutex, portMAX_DELAY);
    if (reply_matches_locked(turn_id, generation) && reply_session.phase == ReplyExpressionPhase::PendingAudio) {
        reply_session.phase = ReplyExpressionPhase::Playing;
        reply_session.last_audio_ms = now_ms();
        snprintf(expression, sizeof(expression), "%s", reply_session.expression);
        show = true;
    }
    xSemaphoreGive(reply_mutex);
    if (show) {
        show_expression(expression);
    }
}

void expression_state_reply_segment_finished(const char* turn_id, uint32_t generation)
{
    if (reply_mutex == nullptr) {
        return;
    }
    xSemaphoreTake(reply_mutex, portMAX_DELAY);
    if (reply_matches_locked(turn_id, generation) &&
        (reply_session.phase == ReplyExpressionPhase::Playing ||
         reply_session.phase == ReplyExpressionPhase::BetweenSegments)) {
        reply_session.phase = ReplyExpressionPhase::BetweenSegments;
        reply_session.last_audio_ms = now_ms();
    }
    xSemaphoreGive(reply_mutex);
}

void expression_state_reply_end(const char* turn_id, uint32_t generation, bool cancelled)
{
    bool restore_calm = false;
    if (reply_mutex == nullptr) {
        return;
    }
    xSemaphoreTake(reply_mutex, portMAX_DELAY);
    if (reply_matches_locked(turn_id, generation)) {
        if (cancelled) {
            clear_reply_locked();
            restore_calm = true;
        } else {
            reply_session.phase = ReplyExpressionPhase::Releasing;
            reply_release_at_ms = now_ms() + kReplyReleaseMs;
        }
    }
    xSemaphoreGive(reply_mutex);
    if (restore_calm) {
        show_expression(kDefaultExpression);
    }
}

void expression_state_reply_failed(const char* turn_id, uint32_t generation)
{
    expression_state_reply_end(turn_id, generation, true);
}

void expression_state_reply_cancel(const char* reason)
{
    bool restore_calm = false;
    if (reply_mutex != nullptr) {
        xSemaphoreTake(reply_mutex, portMAX_DELAY);
        restore_calm = reply_session.phase != ReplyExpressionPhase::None;
        clear_reply_locked();
        xSemaphoreGive(reply_mutex);
    }
    if (restore_calm) {
        show_expression(kDefaultExpression);
        ESP_LOGI(TAG, "Reply expression cancelled: %s", reason != nullptr ? reason : "unspecified");
    }
}
