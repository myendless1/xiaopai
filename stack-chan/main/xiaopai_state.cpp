#include "xiaopai_state.h"



#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

namespace {

static constexpr const char* TAG = "XiaopaiSupervisor";
static constexpr UBaseType_t kSupervisorQueueLength = 32;
static constexpr uint32_t kSupervisorTaskStackBytes = 6 * 1024;
static constexpr UBaseType_t kSupervisorTaskPriority = 6;
static constexpr TickType_t kSupervisorSubmitTimeout = pdMS_TO_TICKS(500);
static constexpr size_t kSupervisorReasonBytes = 64;

volatile SystemMode system_mode = SystemMode::Booting;
volatile InteractionState interaction_state = InteractionState::Monitoring;
volatile MicSource active_source = MicSource::InternalMic;
volatile uint32_t state_generation = 0;
volatile uint32_t cancellation_generation = 0;
volatile uint32_t source_generation = 0;
volatile uint32_t next_job_id = 1;
QueueHandle_t supervisor_queue = nullptr;
TaskHandle_t supervisor_task_handle = nullptr;

enum class SupervisorEventType : uint8_t {
    SetVoiceState,
    BeginRecording,
    EndRecording,
    BeginSpeaking,
    EndSpeaking,
    SetSystemMode,
    CancelCurrent,
    CommitAudioSource,
    AdmitJob,
    MakeJobHeader,
    RunCallback,
};

struct SupervisorEvent {
    SupervisorEventType type = SupervisorEventType::SetVoiceState;
    SupervisorSafetyClass safety_class = SupervisorSafetyClass::Normal;
    SupervisorJobKind job_kind = SupervisorJobKind::Control;
    LocalVoiceState voice_state = LocalVoiceState::Idle;
    SystemMode system_mode = SystemMode::Active;
    MicSource mic_source = MicSource::InternalMic;
    SupervisorCallback callback = nullptr;
    uintptr_t callback_arg = 0;
    JobHeader* job_header = nullptr;
    uint32_t boot_id = 0;
    uint64_t deadline_tick = 0;
    char cmd_id[40] = {};
    char reason[kSupervisorReasonBytes] = {};
    SemaphoreHandle_t done = nullptr;
    bool* result = nullptr;
};

bool xiaopai_state_from_name(const char* name, LocalVoiceState* state)
{
    if (name == nullptr || state == nullptr) {
        return false;
    }
    if (strcmp(name, "idle") == 0) {
        *state = LocalVoiceState::Idle;
        return true;
    }
    if (strcmp(name, "listening") == 0 || strcmp(name, "active") == 0 || strcmp(name, "awake") == 0) {
        *state = LocalVoiceState::Listening;
        return true;
    }
    if (strcmp(name, "dialog_sleeping") == 0 || strcmp(name, "sleep") == 0 || strcmp(name, "sleeping") == 0) {
        *state = LocalVoiceState::DialogSleeping;
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

InteractionState interaction_from_voice_state(LocalVoiceState state)
{
    switch (state) {
        case LocalVoiceState::Idle:
        case LocalVoiceState::Listening:
        case LocalVoiceState::DialogSleeping:
            return InteractionState::Monitoring;
        case LocalVoiceState::Waiting:
            return InteractionState::WaitingReply;
        case LocalVoiceState::Speaking:
            return InteractionState::Speaking;
    }
    return InteractionState::Monitoring;
}

XiaopaiStateSnapshot state_snapshot_unlocked()
{
    XiaopaiStateSnapshot snapshot;
    LocalVoiceState state = local_voice_current_state();
    snapshot.name = local_voice_state_name(state);
    snapshot.system_mode = system_mode;
    snapshot.system_mode_name = xiaopai_system_mode_name(snapshot.system_mode);
    snapshot.interaction_state = interaction_state;
    snapshot.interaction_state_name = xiaopai_interaction_state_name(snapshot.interaction_state);
    snapshot.active_source = active_source;
    snapshot.state = state;
    snapshot.generation = state_generation;
    snapshot.cancellation_generation = cancellation_generation;
    snapshot.source_generation = source_generation;
    snapshot.is_speaking = local_voice_is_speaking();
    snapshot.can_sample_mic = snapshot.system_mode != SystemMode::Maintenance &&
                              snapshot.system_mode != SystemMode::Fault &&
                              snapshot.interaction_state == InteractionState::Monitoring &&
                              local_voice_can_sample_mic();
    return snapshot;
}

void push_state_debug_if_changed(const XiaopaiStateSnapshot& before,
                                 const XiaopaiStateSnapshot& after,
                                 const char* reason)
{
    const bool changed =
        before.generation != after.generation || before.is_speaking != after.is_speaking ||
        before.can_sample_mic != after.can_sample_mic || strcmp(before.name, after.name) != 0 ||
        before.system_mode != after.system_mode || before.interaction_state != after.interaction_state ||
        before.cancellation_generation != after.cancellation_generation ||
        before.source_generation != after.source_generation;

    if (!changed) {
        return;
    }

    ESP_LOGI(TAG,
             "STATE from=%s to=%s mode=%s interaction=%s "
             "generation=%u cancel_generation=%u source_generation=%u "
             "speaking=%d can_sample_mic=%d reason=\"%s\"",
             before.name != nullptr ? before.name : "", after.name != nullptr ? after.name : "",
             after.system_mode_name != nullptr ? after.system_mode_name : "",
             after.interaction_state_name != nullptr ? after.interaction_state_name : "",
             static_cast<unsigned>(after.generation), static_cast<unsigned>(after.cancellation_generation),
             static_cast<unsigned>(after.source_generation), after.is_speaking ? 1 : 0, after.can_sample_mic ? 1 : 0,
             reason != nullptr ? reason : "");
}

void bump_state_generation_if_changed(const XiaopaiStateSnapshot& before)
{
    XiaopaiStateSnapshot after = state_snapshot_unlocked();
    if (before.system_mode != after.system_mode || before.interaction_state != after.interaction_state ||
        before.state != after.state || before.is_speaking != after.is_speaking ||
        before.cancellation_generation != after.cancellation_generation ||
        before.source_generation != after.source_generation) {
        state_generation = state_generation + 1;
    }
}

bool job_allowed_unlocked(SupervisorJobKind kind, SupervisorSafetyClass safety_class)
{
    if (safety_class == SupervisorSafetyClass::LocalStop ||
        safety_class == SupervisorSafetyClass::SafetyStop ||
        safety_class == SupervisorSafetyClass::FaultRecovery) {
        return true;
    }

    if (system_mode == SystemMode::Fault) {
        return kind == SupervisorJobKind::Control || kind == SupervisorJobKind::Ui ||
               kind == SupervisorJobKind::FaultRecovery;
    }
    if (system_mode == SystemMode::Maintenance) {
        return kind == SupervisorJobKind::Control || kind == SupervisorJobKind::Ui ||
               kind == SupervisorJobKind::Maintenance || kind == SupervisorJobKind::FaultRecovery;
    }
    if (system_mode == SystemMode::Quiet) {
        return kind == SupervisorJobKind::AudioInput || kind == SupervisorJobKind::Control ||
               kind == SupervisorJobKind::Ui;
    }
    if (system_mode == SystemMode::Booting) {
        return kind == SupervisorJobKind::Control || kind == SupervisorJobKind::Ui ||
               kind == SupervisorJobKind::Maintenance || kind == SupervisorJobKind::FaultRecovery;
    }

    if (kind == SupervisorJobKind::Speech && interaction_state == InteractionState::Speaking) {
        return false;
    }
    if (kind == SupervisorJobKind::AudioInput && interaction_state != InteractionState::Monitoring) {
        return false;
    }
    return true;
}

void cancel_current_unlocked(const char* reason)
{
    cancellation_generation = cancellation_generation + 1;
    local_voice_request_state(LocalVoiceState::Idle, reason);
    interaction_state = InteractionState::Monitoring;
}

bool apply_set_voice_state_unlocked(LocalVoiceState state, const char* reason)
{
    XiaopaiStateSnapshot before = state_snapshot_unlocked();
    InteractionState next_interaction = interaction_from_voice_state(state);
    if ((system_mode == SystemMode::Maintenance || system_mode == SystemMode::Fault) &&
        state != LocalVoiceState::Idle) {
        ESP_LOGW(TAG, "State rejected by supervisor gate: state=%s mode=%s reason=%s",
                 local_voice_state_name(state), xiaopai_system_mode_name(system_mode),
                 reason != nullptr ? reason : "");
        return false;
    }
    SupervisorJobKind job_kind = SupervisorJobKind::Control;
    if (next_interaction == InteractionState::Speaking || next_interaction == InteractionState::WaitingReply) {
        job_kind = SupervisorJobKind::Speech;
    }
    if (!job_allowed_unlocked(job_kind, SupervisorSafetyClass::Normal)) {
        ESP_LOGW(TAG, "State rejected by supervisor gate: state=%s mode=%s reason=%s",
                 local_voice_state_name(state), xiaopai_system_mode_name(system_mode),
                 reason != nullptr ? reason : "");
        return false;
    }
    if (system_mode == SystemMode::Quiet && next_interaction != InteractionState::Monitoring) {
        system_mode = SystemMode::Active;
    }
    local_voice_request_state(state, reason);
    interaction_state = interaction_from_voice_state(local_voice_current_state());
    bump_state_generation_if_changed(before);
    XiaopaiStateSnapshot after = state_snapshot_unlocked();
    push_state_debug_if_changed(before, after, reason);
    return true;
}

bool apply_begin_recording_unlocked(const char* reason)
{
    if (!job_allowed_unlocked(SupervisorJobKind::AudioInput, SupervisorSafetyClass::Normal)) {
        ESP_LOGW(TAG, "Recording rejected by supervisor gate: mode=%s interaction=%s reason=%s",
                 xiaopai_system_mode_name(system_mode), xiaopai_interaction_state_name(interaction_state),
                 reason != nullptr ? reason : "");
        return false;
    }
    XiaopaiStateSnapshot before = state_snapshot_unlocked();
    interaction_state = InteractionState::Recording;
    bump_state_generation_if_changed(before);
    XiaopaiStateSnapshot after = state_snapshot_unlocked();
    push_state_debug_if_changed(before, after, reason);
    return true;
}

bool apply_end_recording_unlocked(const char* reason)
{
    XiaopaiStateSnapshot before = state_snapshot_unlocked();
    if (interaction_state == InteractionState::Recording) {
        interaction_state = InteractionState::WaitingReply;
    }
    bump_state_generation_if_changed(before);
    XiaopaiStateSnapshot after = state_snapshot_unlocked();
    push_state_debug_if_changed(before, after, reason);
    return true;
}

bool apply_begin_speaking_unlocked(const char* reason)
{
    if (!job_allowed_unlocked(SupervisorJobKind::Speech, SupervisorSafetyClass::Normal)) {
        ESP_LOGW(TAG, "Speech rejected by supervisor gate: mode=%s interaction=%s reason=%s",
                 xiaopai_system_mode_name(system_mode), xiaopai_interaction_state_name(interaction_state),
                 reason != nullptr ? reason : "");
        return false;
    }
    XiaopaiStateSnapshot before = state_snapshot_unlocked();
    interaction_state = InteractionState::Speaking;
    local_voice_begin_speaking(reason);
    bump_state_generation_if_changed(before);
    XiaopaiStateSnapshot after = state_snapshot_unlocked();
    push_state_debug_if_changed(before, after, reason);
    return true;
}

bool apply_end_speaking_unlocked(const char* reason)
{
    XiaopaiStateSnapshot before = state_snapshot_unlocked();
    local_voice_end_speaking(reason);
    interaction_state = interaction_from_voice_state(local_voice_current_state());
    bump_state_generation_if_changed(before);
    XiaopaiStateSnapshot after = state_snapshot_unlocked();
    push_state_debug_if_changed(before, after, reason);
    return true;
}

bool apply_system_mode_set_unlocked(SystemMode mode, const char* reason)
{
    XiaopaiStateSnapshot before = state_snapshot_unlocked();
    if (system_mode == mode) {
        return true;
    }
    system_mode = mode;
    if (mode == SystemMode::Quiet) {
        interaction_state = InteractionState::Monitoring;
        local_voice_request_state(LocalVoiceState::Idle, reason);
    } else if (mode == SystemMode::Maintenance || mode == SystemMode::Fault) {
        cancel_current_unlocked(reason);
    }
    bump_state_generation_if_changed(before);
    XiaopaiStateSnapshot after = state_snapshot_unlocked();
    push_state_debug_if_changed(before, after, reason);
    return true;
}

bool apply_cancel_current_unlocked(const char* reason)
{
    XiaopaiStateSnapshot before = state_snapshot_unlocked();
    cancel_current_unlocked(reason);
    bump_state_generation_if_changed(before);
    XiaopaiStateSnapshot after = state_snapshot_unlocked();
    push_state_debug_if_changed(before, after, reason);
    return true;
}

bool apply_audio_source_commit_unlocked(MicSource source, const char* reason)
{
    if (interaction_state == InteractionState::Recording) {
        ESP_LOGW(TAG, "Mic source switch rejected while recording: reason=%s",
                 reason != nullptr ? reason : "");
        return false;
    }
    if (active_source == source) {
        return true;
    }
    XiaopaiStateSnapshot before = state_snapshot_unlocked();
    active_source = source;
    source_generation = source_generation + 1;
    bump_state_generation_if_changed(before);
    XiaopaiStateSnapshot after = state_snapshot_unlocked();
    push_state_debug_if_changed(before, after, reason);
    return true;
}

bool apply_make_job_header_unlocked(const SupervisorEvent& event)
{
    if (event.job_header == nullptr) {
        return false;
    }
    XiaopaiStateSnapshot snapshot = state_snapshot_unlocked();
    JobHeader header;
    strncpy(header.cmd_id, event.cmd_id, sizeof(header.cmd_id) - 1);
    header.job_id = next_job_id++;
    header.boot_id = event.boot_id;
    header.state_generation = snapshot.generation;
    header.cancellation_generation = snapshot.cancellation_generation;
    header.source_generation = snapshot.source_generation;
    header.deadline_tick = event.deadline_tick;
    *event.job_header = header;
    return true;
}

bool apply_event_unlocked(const SupervisorEvent& event)
{
    const char* reason = event.reason[0] != '\0' ? event.reason : nullptr;
    switch (event.type) {
        case SupervisorEventType::SetVoiceState:
            return apply_set_voice_state_unlocked(event.voice_state, reason);
        case SupervisorEventType::BeginRecording:
            return apply_begin_recording_unlocked(reason);
        case SupervisorEventType::EndRecording:
            return apply_end_recording_unlocked(reason);
        case SupervisorEventType::BeginSpeaking:
            return apply_begin_speaking_unlocked(reason);
        case SupervisorEventType::EndSpeaking:
            return apply_end_speaking_unlocked(reason);
        case SupervisorEventType::SetSystemMode:
            return apply_system_mode_set_unlocked(event.system_mode, reason);
        case SupervisorEventType::CancelCurrent:
            return apply_cancel_current_unlocked(reason);
        case SupervisorEventType::CommitAudioSource:
            return apply_audio_source_commit_unlocked(event.mic_source, reason);
        case SupervisorEventType::AdmitJob:
            return job_allowed_unlocked(event.job_kind, event.safety_class);
        case SupervisorEventType::MakeJobHeader:
            return apply_make_job_header_unlocked(event);
        case SupervisorEventType::RunCallback:
            if (!job_allowed_unlocked(event.job_kind, event.safety_class)) {
                ESP_LOGW(TAG, "Callback rejected by supervisor gate: mode=%s reason=%s",
                         xiaopai_system_mode_name(system_mode), reason != nullptr ? reason : "");
                return false;
            }
            if (event.safety_class == SupervisorSafetyClass::LocalStop ||
                event.safety_class == SupervisorSafetyClass::SafetyStop ||
                event.safety_class == SupervisorSafetyClass::FaultRecovery) {
                apply_cancel_current_unlocked(reason);
            }
            if (event.callback != nullptr) {
                event.callback(event.callback_arg);
            }
            return true;
    }
    return false;
}

void supervisor_task(void*)
{
    while (true) {
        SupervisorEvent event;
        if (xQueueReceive(supervisor_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        bool result = apply_event_unlocked(event);
        if (event.result != nullptr) {
            *event.result = result;
        }
        if (event.done != nullptr) {
            xSemaphoreGive(event.done);
        }
    }
}

bool supervisor_submit(SupervisorEvent& event, bool wait)
{
    if (supervisor_queue == nullptr || supervisor_task_handle == nullptr ||
        xTaskGetCurrentTaskHandle() == supervisor_task_handle) {
        return apply_event_unlocked(event);
    }

    bool result = false;
    StaticSemaphore_t done_buffer = {};
    SemaphoreHandle_t done = nullptr;
    if (wait) {
        done = xSemaphoreCreateBinaryStatic(&done_buffer);
        event.done = done;
        event.result = &result;
    }

    const bool priority_event = event.type == SupervisorEventType::CancelCurrent ||
                                event.safety_class == SupervisorSafetyClass::LocalStop ||
                                event.safety_class == SupervisorSafetyClass::SafetyStop ||
                                event.safety_class == SupervisorSafetyClass::FaultRecovery;
    BaseType_t queued = priority_event ? xQueueSendToFront(supervisor_queue, &event, kSupervisorSubmitTimeout)
                                       : xQueueSend(supervisor_queue, &event, kSupervisorSubmitTimeout);
    if (queued != pdTRUE) {
        ESP_LOGE(TAG, "SupervisorQueue full; dropping event type=%u reason=%s",
                 static_cast<unsigned>(event.type), event.reason);
        return false;
    }

    if (!wait) {
        return true;
    }
    xSemaphoreTake(done, portMAX_DELAY);
    return result;
}

bool supervisor_start()
{
    if (supervisor_queue == nullptr) {
        supervisor_queue = xQueueCreate(kSupervisorQueueLength, sizeof(SupervisorEvent));
        if (supervisor_queue == nullptr) {
            ESP_LOGE(TAG, "Failed to create SupervisorQueue");
            return false;
        }
    }
    if (supervisor_task_handle == nullptr) {
        BaseType_t created = xTaskCreatePinnedToCore(
            supervisor_task,
            "supervisor_task",
            kSupervisorTaskStackBytes,
            nullptr,
            kSupervisorTaskPriority,
            &supervisor_task_handle,
            1);
        if (created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create supervisor_task");
            supervisor_task_handle = nullptr;
            return false;
        }
    }
    return true;
}

void set_reason(SupervisorEvent& event, const char* reason)
{
    if (reason != nullptr) {
        strncpy(event.reason, reason, sizeof(event.reason) - 1);
    }
}

} // namespace

void xiaopai_state_init(const LocalVoiceStateHooks& voice_hooks)
{
    local_voice_state_init(voice_hooks);
    supervisor_start();
    xiaopai_system_mode_set(SystemMode::Active, "init");
}

TaskHandle_t xiaopai_supervisor_get_task_handle()
{
    return supervisor_task_handle;
}

XiaopaiStateSnapshot xiaopai_state_get()
{
    return state_snapshot_unlocked();
}

const char* xiaopai_state_name(LocalVoiceState state)
{
    return local_voice_state_name(state);
}

const char* xiaopai_system_mode_name(SystemMode mode)
{
    switch (mode) {
        case SystemMode::Booting:
            return "booting";
        case SystemMode::Active:
            return "active";
        case SystemMode::Quiet:
            return "quiet";
        case SystemMode::Maintenance:
            return "maintenance";
        case SystemMode::Fault:
            return "fault";
    }
    return "unknown";
}

const char* xiaopai_interaction_state_name(InteractionState state)
{
    switch (state) {
        case InteractionState::Monitoring:
            return "monitoring";
        case InteractionState::Recording:
            return "recording";
        case InteractionState::WaitingReply:
            return "waiting_reply";
        case InteractionState::Speaking:
            return "speaking";
    }
    return "unknown";
}

bool xiaopai_state_set(LocalVoiceState state, const char* reason)
{
    SupervisorEvent event;
    event.type = SupervisorEventType::SetVoiceState;
    event.voice_state = state;
    set_reason(event, reason);
    return supervisor_submit(event, true);
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

bool xiaopai_state_begin_recording(const char* reason)
{
    SupervisorEvent event;
    event.type = SupervisorEventType::BeginRecording;
    set_reason(event, reason);
    return supervisor_submit(event, true);
}

bool xiaopai_state_end_recording(const char* reason)
{
    SupervisorEvent event;
    event.type = SupervisorEventType::EndRecording;
    set_reason(event, reason);
    return supervisor_submit(event, true);
}

bool xiaopai_state_begin_speaking(const char* reason)
{
    SupervisorEvent event;
    event.type = SupervisorEventType::BeginSpeaking;
    set_reason(event, reason);
    return supervisor_submit(event, true);
}

bool xiaopai_state_end_speaking(const char* reason)
{
    SupervisorEvent event;
    event.type = SupervisorEventType::EndSpeaking;
    set_reason(event, reason);
    return supervisor_submit(event, true);
}

bool xiaopai_system_mode_set(SystemMode mode, const char* reason)
{
    SupervisorEvent event;
    event.type = SupervisorEventType::SetSystemMode;
    event.system_mode = mode;
    set_reason(event, reason);
    return supervisor_submit(event, true);
}

bool xiaopai_cancel_current(const char* reason)
{
    SupervisorEvent event;
    event.type = SupervisorEventType::CancelCurrent;
    event.safety_class = SupervisorSafetyClass::SafetyStop;
    set_reason(event, reason);
    return supervisor_submit(event, true);
}

bool xiaopai_audio_source_commit(MicSource source, const char* reason)
{
    SupervisorEvent event;
    event.type = SupervisorEventType::CommitAudioSource;
    event.mic_source = source;
    set_reason(event, reason);
    return supervisor_submit(event, true);
}

JobHeader xiaopai_make_job_header(const char* cmd_id, uint32_t boot_id, uint64_t deadline_tick)
{
    JobHeader header;
    SupervisorEvent event;
    event.type = SupervisorEventType::MakeJobHeader;
    event.job_header = &header;
    event.boot_id = boot_id;
    event.deadline_tick = deadline_tick;
    if (cmd_id != nullptr) {
        strncpy(event.cmd_id, cmd_id, sizeof(event.cmd_id) - 1);
    }
    supervisor_submit(event, true);
    return header;
}

bool xiaopai_job_is_current(const JobHeader& header, uint64_t now_tick)
{
    XiaopaiStateSnapshot snapshot = xiaopai_state_get();
    if (header.state_generation != snapshot.generation) {
        return false;
    }
    if (header.cancellation_generation != snapshot.cancellation_generation) {
        return false;
    }
    if (header.source_generation != snapshot.source_generation) {
        return false;
    }
    return header.deadline_tick == 0 || header.deadline_tick >= now_tick;
}

bool xiaopai_supervisor_admit(SupervisorJobKind kind, SupervisorSafetyClass safety_class, const char* reason)
{
    SupervisorEvent event;
    event.type = SupervisorEventType::AdmitJob;
    event.job_kind = kind;
    event.safety_class = safety_class;
    set_reason(event, reason);
    return supervisor_submit(event, true);
}

bool xiaopai_supervisor_run(SupervisorCallback callback, uintptr_t arg, SupervisorSafetyClass safety_class,
                            const char* reason)
{
    SupervisorEvent event;
    event.type = SupervisorEventType::RunCallback;
    event.callback = callback;
    event.callback_arg = arg;
    event.safety_class = safety_class;
    event.job_kind = SupervisorJobKind::Control;
    set_reason(event, reason);
    return supervisor_submit(event, true);
}
