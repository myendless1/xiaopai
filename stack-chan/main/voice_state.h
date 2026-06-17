#pragma once

#include <stdint.h>

enum class LocalVoiceState : uint8_t {
    Idle,
    Listening,
    Waiting,
    Speaking,
};

struct LocalVoiceStateHooks {
    void (*set_sleeping)();
    void (*set_listening)();
    void (*set_waiting)();
    void (*set_speaking)();
    void (*on_idle_to_listening)(const char* reason);
};

void local_voice_state_init(const LocalVoiceStateHooks& hooks);
const char* local_voice_state_name(LocalVoiceState state);
LocalVoiceState local_voice_current_state();
uint32_t local_voice_generation();
bool local_voice_is_speaking();
bool local_voice_can_sample_mic();
void local_voice_apply_outputs(LocalVoiceState state);
void local_voice_request_state(LocalVoiceState new_state, const char* reason);
void local_voice_begin_speaking(const char* reason);
void local_voice_end_speaking(const char* reason);
