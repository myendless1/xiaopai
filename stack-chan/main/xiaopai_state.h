#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "voice_state.h"

#include <stdint.h>

enum class SystemMode : uint8_t {
    Booting,
    Active,
    Quiet,
    Maintenance,
    Fault,
};

enum class InteractionState : uint8_t {
    Monitoring,
    Recording,
    WaitingReply,
    Speaking,
};

enum class MicSource : uint8_t {
    InternalMic,
    DjiMic,
};

struct JobHeader {
    char cmd_id[40] = {};
    uint32_t job_id = 0;
    uint32_t boot_id = 0;
    uint32_t state_generation = 0;
    uint32_t cancellation_generation = 0;
    uint32_t source_generation = 0;
    uint32_t turn_id = 0;
    uint64_t deadline_tick = 0;
};

struct XiaopaiStateSnapshot {
    const char* name = "";
    const char* system_mode_name = "";
    const char* interaction_state_name = "";
    LocalVoiceState state = LocalVoiceState::Idle;
    SystemMode system_mode = SystemMode::Booting;
    InteractionState interaction_state = InteractionState::Monitoring;
    MicSource active_source = MicSource::InternalMic;
    uint32_t generation = 0;
    uint32_t cancellation_generation = 0;
    uint32_t source_generation = 0;
    bool is_speaking = false;
    bool can_sample_mic = false;
};

enum class SupervisorJobKind : uint8_t {
    AudioInput,
    Speech,
    Vision,
    Motion,
    Ui,
    Control,
    Maintenance,
    FaultRecovery,
};

enum class SupervisorSafetyClass : uint8_t {
    Normal,
    LocalStop,
    SafetyStop,
    FaultRecovery,
};

using SupervisorCallback = void (*)(uintptr_t arg);

void xiaopai_state_init(const LocalVoiceStateHooks& hooks);
XiaopaiStateSnapshot xiaopai_state_get();
const char* xiaopai_state_name(LocalVoiceState state);
const char* xiaopai_system_mode_name(SystemMode mode);
const char* xiaopai_interaction_state_name(InteractionState state);
bool xiaopai_state_set(LocalVoiceState state, const char* reason = nullptr);
bool xiaopai_state_set(const char* state, const char* reason = nullptr);
void xiaopai_state_apply_outputs(LocalVoiceState state);
bool xiaopai_state_begin_recording(const char* reason);
bool xiaopai_state_end_recording(const char* reason);
bool xiaopai_state_begin_speaking(const char* reason);
bool xiaopai_state_end_speaking(const char* reason);
bool xiaopai_system_mode_set(SystemMode mode, const char* reason = nullptr);
bool xiaopai_cancel_current(const char* reason = nullptr);
bool xiaopai_audio_source_commit(MicSource source, const char* reason = nullptr);
JobHeader xiaopai_make_job_header(const char* cmd_id, uint32_t boot_id, uint64_t deadline_tick);
bool xiaopai_job_is_current(const JobHeader& header, uint64_t now_tick);
bool xiaopai_supervisor_admit(SupervisorJobKind kind, SupervisorSafetyClass safety_class,
                              const char* reason = nullptr);
bool xiaopai_supervisor_run(SupervisorCallback callback, uintptr_t arg,
                            SupervisorSafetyClass safety_class = SupervisorSafetyClass::Normal,
                            const char* reason = nullptr);
TaskHandle_t xiaopai_supervisor_get_task_handle();
