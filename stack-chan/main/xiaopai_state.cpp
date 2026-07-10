#include "xiaopai_state.h"

#include "debug_events.h"

#include <string.h>

namespace {

volatile SystemMode system_mode = SystemMode::Booting;
volatile InteractionState interaction_state = InteractionState::Monitoring;
volatile MicSource active_source = MicSource::InternalMic;
volatile uint32_t cancellation_generation = 0;
volatile uint32_t source_generation = 0;
volatile uint32_t next_job_id = 1;

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

InteractionState interaction_from_voice_state(LocalVoiceState state)
{
    switch (state) {
        case LocalVoiceState::Idle:
        case LocalVoiceState::Listening:
            return InteractionState::Monitoring;
        case LocalVoiceState::Waiting:
            return InteractionState::WaitingReply;
        case LocalVoiceState::Speaking:
            return InteractionState::Speaking;
    }
    return InteractionState::Monitoring;
}

} // namespace

void xiaopai_state_init(const LocalVoiceStateHooks& voice_hooks)
{
    local_voice_state_init(voice_hooks);
    xiaopai_system_mode_set(SystemMode::Active, "init");
}

XiaopaiStateSnapshot xiaopai_state_get()
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
    snapshot.generation = local_voice_generation();
    snapshot.cancellation_generation = cancellation_generation;
    snapshot.source_generation = source_generation;
    snapshot.is_speaking = local_voice_is_speaking();
    snapshot.can_sample_mic = snapshot.system_mode != SystemMode::Maintenance &&
                              snapshot.system_mode != SystemMode::Fault &&
                              local_voice_can_sample_mic();
    return snapshot;
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
    XiaopaiStateSnapshot before = xiaopai_state_get();
    InteractionState next_interaction = interaction_from_voice_state(state);
    if (system_mode == SystemMode::Quiet && next_interaction != InteractionState::Monitoring) {
        system_mode = SystemMode::Active;
    }
    local_voice_request_state(state, reason);
    interaction_state = interaction_from_voice_state(local_voice_current_state());
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
    interaction_state = InteractionState::Speaking;
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
    interaction_state = interaction_from_voice_state(local_voice_current_state());
    XiaopaiStateSnapshot after = xiaopai_state_get();
    if (before.generation != after.generation || before.is_speaking != after.is_speaking ||
        before.can_sample_mic != after.can_sample_mic || strcmp(before.name, after.name) != 0) {
        debug_events_push_xiaopai_state(before.name, after.name, after.generation, after.is_speaking,
                                        after.can_sample_mic, reason);
    }
}

void xiaopai_system_mode_set(SystemMode mode, const char* reason)
{
    XiaopaiStateSnapshot before = xiaopai_state_get();
    if (system_mode == mode) {
        return;
    }
    system_mode = mode;
    if (mode == SystemMode::Quiet) {
        interaction_state = InteractionState::Monitoring;
        local_voice_request_state(LocalVoiceState::Idle, reason);
    } else if (mode == SystemMode::Maintenance || mode == SystemMode::Fault) {
        xiaopai_cancel_current(reason);
        interaction_state = InteractionState::Monitoring;
    }
    XiaopaiStateSnapshot after = xiaopai_state_get();
    debug_events_push_xiaopai_state(before.name, after.name, after.generation, after.is_speaking,
                                    after.can_sample_mic, reason);
}

void xiaopai_cancel_current(const char* reason)
{
    cancellation_generation = cancellation_generation + 1;
    local_voice_request_state(LocalVoiceState::Idle, reason);
    interaction_state = InteractionState::Monitoring;
}

void xiaopai_audio_source_commit(MicSource source, const char* reason)
{
    if (active_source == source) {
        return;
    }
    active_source = source;
    source_generation = source_generation + 1;
    debug_events_push_xiaopai_state(local_voice_state_name(local_voice_current_state()),
                                    local_voice_state_name(local_voice_current_state()),
                                    local_voice_generation(), local_voice_is_speaking(),
                                    local_voice_can_sample_mic(), reason);
}

JobHeader xiaopai_make_job_header(const char* cmd_id, uint32_t boot_id, uint64_t deadline_tick)
{
    XiaopaiStateSnapshot snapshot = xiaopai_state_get();
    JobHeader header;
    if (cmd_id != nullptr) {
        strncpy(header.cmd_id, cmd_id, sizeof(header.cmd_id) - 1);
    }
    header.job_id = next_job_id++;
    header.boot_id = boot_id;
    header.state_generation = snapshot.generation;
    header.cancellation_generation = snapshot.cancellation_generation;
    header.source_generation = snapshot.source_generation;
    header.deadline_tick = deadline_tick;
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
