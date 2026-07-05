#include "xiaopai_state.h"

#include "debug_events.h"

#include <string.h>

namespace {

bool xiaopai_state_from_name(const char* name, LocalVoiceState* state)
{
    if (name == nullptr || state == nullptr) {
        return false;
    }
    if (strcmp(name, "idle") == 0 || strcmp(name, "sleep") == 0 || strcmp(name, "sleeping") == 0) {
        *state = LocalVoiceState::Idle;
        return true;
    }
    if (strcmp(name, "listening") == 0 || strcmp(name, "active") == 0 || strcmp(name, "awake") == 0) {
        *state = LocalVoiceState::Listening;
        return true;
    }
    if (strcmp(name, "waiting") == 0 || strcmp(name, "thinking") == 0) {
        *state = LocalVoiceState::Waiting;
        return true;
    }
    if (strcmp(name, "speaking") == 0) {
        *state = LocalVoiceState::Speaking;
        return true;
    }
    return false;
}

} // namespace

void xiaopai_state_init(const LocalVoiceStateHooks& voice_hooks)
{
    local_voice_state_init(voice_hooks);
}

XiaopaiStateSnapshot xiaopai_state_get()
{
    XiaopaiStateSnapshot snapshot;
    LocalVoiceState state = local_voice_current_state();
    snapshot.name = local_voice_state_name(state);
    snapshot.state = state;
    snapshot.generation = local_voice_generation();
    snapshot.is_speaking = local_voice_is_speaking();
    snapshot.can_sample_mic = local_voice_can_sample_mic();
    return snapshot;
}

const char* xiaopai_state_name(LocalVoiceState state)
{
    return local_voice_state_name(state);
}

bool xiaopai_state_set(LocalVoiceState state, const char* reason)
{
    XiaopaiStateSnapshot before = xiaopai_state_get();
    local_voice_request_state(state, reason);
    XiaopaiStateSnapshot after = xiaopai_state_get();
    if (before.generation != after.generation || before.is_speaking != after.is_speaking ||
        before.can_sample_mic != after.can_sample_mic || strcmp(before.name, after.name) != 0) {
        debug_events_push_xiaopai_state(before.name, after.name, after.generation, after.is_speaking,
                                        after.can_sample_mic, reason);
    }
    return true;
}

bool xiaopai_state_set(const char* state, const char* reason)
{
    LocalVoiceState parsed_state = LocalVoiceState::Idle;
    if (!xiaopai_state_from_name(state, &parsed_state)) {
        return false;
    }
    return xiaopai_state_set(parsed_state, reason);
}

void xiaopai_state_apply_outputs(LocalVoiceState state)
{
    local_voice_apply_outputs(state);
}

void xiaopai_state_begin_speaking(const char* reason)
{
    XiaopaiStateSnapshot before = xiaopai_state_get();
    local_voice_begin_speaking(reason);
    XiaopaiStateSnapshot after = xiaopai_state_get();
    if (before.generation != after.generation || before.is_speaking != after.is_speaking ||
        before.can_sample_mic != after.can_sample_mic || strcmp(before.name, after.name) != 0) {
        debug_events_push_xiaopai_state(before.name, after.name, after.generation, after.is_speaking,
                                        after.can_sample_mic, reason);
    }
}

void xiaopai_state_end_speaking(const char* reason)
{
    XiaopaiStateSnapshot before = xiaopai_state_get();
    local_voice_end_speaking(reason);
    XiaopaiStateSnapshot after = xiaopai_state_get();
    if (before.generation != after.generation || before.is_speaking != after.is_speaking ||
        before.can_sample_mic != after.can_sample_mic || strcmp(before.name, after.name) != 0) {
        debug_events_push_xiaopai_state(before.name, after.name, after.generation, after.is_speaking,
                                        after.can_sample_mic, reason);
    }
}
