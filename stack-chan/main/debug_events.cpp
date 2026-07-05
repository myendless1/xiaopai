#include "debug_events.h"

#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

namespace {

static constexpr UBaseType_t kDebugEventQueueDepth = 32;
QueueHandle_t debug_event_queue = nullptr;

void copy_field(char* dest, size_t dest_size, const char* value)
{
    if (dest == nullptr || dest_size == 0) {
        return;
    }
    const char* text = value != nullptr ? value : "";
    snprintf(dest, dest_size, "%s", text);
}

uint32_t now_ms()
{
    if (xPortInIsrContext()) {
        return static_cast<uint32_t>(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS);
    }
    return static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

bool push_event(const DebugEventItem& item)
{
    if (debug_event_queue == nullptr) {
        return false;
    }
    if (xPortInIsrContext()) {
        BaseType_t high_task_woken = pdFALSE;
        BaseType_t ok = xQueueSendFromISR(debug_event_queue, &item, &high_task_woken);
        if (high_task_woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
        return ok == pdTRUE;
    }
    return xQueueSend(debug_event_queue, &item, 0) == pdTRUE;
}

} // namespace

bool debug_events_init()
{
    if (debug_event_queue != nullptr) {
        return true;
    }
    debug_event_queue = xQueueCreate(kDebugEventQueueDepth, sizeof(DebugEventItem));
    return debug_event_queue != nullptr;
}

bool debug_events_push_log_line(const char* line)
{
    if (line == nullptr || line[0] == '\0') {
        return false;
    }
    DebugEventItem item;
    item.kind = DebugEventKind::LogLine;
    item.device_ms = now_ms();
    copy_field(item.source, sizeof(item.source), "esp-log");
    copy_field(item.line, sizeof(item.line), line);
    return push_event(item);
}

bool debug_events_push_xiaopai_state(const char* from, const char* to, uint32_t generation,
                                     bool is_speaking, bool can_sample_mic, const char* reason)
{
    DebugEventItem item;
    item.kind = DebugEventKind::StateChange;
    item.device_ms = now_ms();
    copy_field(item.source, sizeof(item.source), "xiaopai-state");
    copy_field(item.from, sizeof(item.from), from);
    copy_field(item.to, sizeof(item.to), to);
    copy_field(item.reason, sizeof(item.reason), reason);
    item.generation = generation;
    item.is_speaking = is_speaking;
    item.can_sample_mic = can_sample_mic;
    return push_event(item);
}

bool debug_events_push_expression_state(const char* from, const char* to, bool screen_visible,
                                        bool sleep_dark, bool animation_active, const char* reason)
{
    DebugEventItem item;
    item.kind = DebugEventKind::StateChange;
    item.device_ms = now_ms();
    copy_field(item.source, sizeof(item.source), "expression-state");
    copy_field(item.from, sizeof(item.from), from);
    copy_field(item.to, sizeof(item.to), to);
    copy_field(item.reason, sizeof(item.reason), reason);
    item.screen_visible = screen_visible;
    item.sleep_dark = sleep_dark;
    item.animation_active = animation_active;
    return push_event(item);
}

bool debug_events_pop(DebugEventItem* out, TickType_t timeout_ticks)
{
    if (out == nullptr || debug_event_queue == nullptr) {
        return false;
    }
    return xQueueReceive(debug_event_queue, out, timeout_ticks) == pdTRUE;
}
