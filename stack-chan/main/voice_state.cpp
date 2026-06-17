#include "voice_state.h"

#include "esp_log.h"

namespace {

static constexpr const char* TAG = "Xiaopai";

LocalVoiceStateHooks hooks = {};
volatile LocalVoiceState state = LocalVoiceState::Idle;
volatile LocalVoiceState return_state = LocalVoiceState::Idle;
volatile int speaking_depth = 0;
volatile uint32_t generation = 0;

void maybe_notify_idle_to_listening(LocalVoiceState old_state, LocalVoiceState new_state, const char* reason)
{
    if (old_state == LocalVoiceState::Idle && new_state == LocalVoiceState::Listening &&
        hooks.on_idle_to_listening != nullptr) {
        hooks.on_idle_to_listening(reason);
    }
}

} // namespace

void local_voice_state_init(const LocalVoiceStateHooks& new_hooks)
{
    hooks = new_hooks;
}

const char* local_voice_state_name(LocalVoiceState state_value)
{
    switch (state_value) {
        case LocalVoiceState::Idle:
            return "idle";
        case LocalVoiceState::Listening:
            return "listening";
        case LocalVoiceState::Waiting:
            return "waiting";
        case LocalVoiceState::Speaking:
            return "speaking";
    }
    return "unknown";
}

LocalVoiceState local_voice_current_state()
{
    return state;
}

uint32_t local_voice_generation()
{
    return generation;
}

bool local_voice_is_speaking()
{
    return state == LocalVoiceState::Speaking;
}

bool local_voice_can_sample_mic()
{
    return state == LocalVoiceState::Idle || state == LocalVoiceState::Listening;
}

void local_voice_apply_outputs(LocalVoiceState state_value)
{
    if (state_value == LocalVoiceState::Idle) {
        if (hooks.set_sleeping != nullptr) {
            hooks.set_sleeping();
        }
    } else if (state_value == LocalVoiceState::Listening) {
        if (hooks.set_listening != nullptr) {
            hooks.set_listening();
        }
    } else if (state_value == LocalVoiceState::Waiting) {
        if (hooks.set_waiting != nullptr) {
            hooks.set_waiting();
        }
    } else {
        if (hooks.set_speaking != nullptr) {
            hooks.set_speaking();
        }
    }
}

void local_voice_request_state(LocalVoiceState new_state, const char* reason)
{
    LocalVoiceState old_state = state;
    if (old_state == LocalVoiceState::Speaking && new_state != LocalVoiceState::Speaking &&
        speaking_depth > 0) {
        return_state = new_state;
        ESP_LOGI(TAG, "Local voice state deferred while speaking: return=%s reason=%s",
                 local_voice_state_name(new_state), reason != nullptr ? reason : "");
        return;
    }
    if (old_state == new_state) {
        local_voice_apply_outputs(new_state);
        return;
    }

    state = new_state;
    generation = generation + 1;
    ESP_LOGI(TAG, "Local voice state: %s -> %s reason=%s", local_voice_state_name(old_state),
             local_voice_state_name(new_state), reason != nullptr ? reason : "");
    maybe_notify_idle_to_listening(old_state, new_state, reason);
    local_voice_apply_outputs(new_state);
}

void local_voice_begin_speaking(const char* reason)
{
    speaking_depth = speaking_depth + 1;
    if (speaking_depth > 1) {
        ESP_LOGI(TAG, "Local voice speaking nested depth=%d reason=%s", speaking_depth,
                 reason != nullptr ? reason : "");
        return;
    }

    LocalVoiceState old_state = state;
    return_state = old_state == LocalVoiceState::Speaking ? LocalVoiceState::Listening : old_state;
    state = LocalVoiceState::Speaking;
    generation = generation + 1;
    ESP_LOGI(TAG, "Local voice state: %s -> speaking reason=%s", local_voice_state_name(old_state),
             reason != nullptr ? reason : "");
    local_voice_apply_outputs(LocalVoiceState::Speaking);
}

void local_voice_end_speaking(const char* reason)
{
    if (speaking_depth > 0) {
        speaking_depth = speaking_depth - 1;
    }
    if (speaking_depth > 0) {
        ESP_LOGI(TAG, "Local voice speaking still nested depth=%d reason=%s", speaking_depth,
                 reason != nullptr ? reason : "");
        return;
    }

    LocalVoiceState next_state = return_state;
    if (next_state == LocalVoiceState::Speaking) {
        next_state = LocalVoiceState::Listening;
    }
    LocalVoiceState old_state = state;
    state = next_state;
    generation = generation + 1;
    ESP_LOGI(TAG, "Local voice state: %s -> %s reason=%s", local_voice_state_name(old_state),
             local_voice_state_name(next_state), reason != nullptr ? reason : "");
    local_voice_apply_outputs(next_state);
}
