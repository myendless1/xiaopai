#pragma once

#include "voice_state.h"

#include <stdint.h>

struct XiaopaiStateSnapshot {
    const char* name = "";
    LocalVoiceState state = LocalVoiceState::Idle;
    uint32_t generation = 0;
    bool is_speaking = false;
    bool can_sample_mic = false;
};

void xiaopai_state_init(const LocalVoiceStateHooks& hooks);
XiaopaiStateSnapshot xiaopai_state_get();
const char* xiaopai_state_name(LocalVoiceState state);
bool xiaopai_state_set(LocalVoiceState state, const char* reason = nullptr);
bool xiaopai_state_set(const char* state, const char* reason = nullptr);
void xiaopai_state_apply_outputs(LocalVoiceState state);
void xiaopai_state_begin_speaking(const char* reason);
void xiaopai_state_end_speaking(const char* reason);
