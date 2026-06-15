## ADDED Requirements

### Requirement: Xiaopai render intent is detected from stack-chan envelopes
The system SHALL detect Xiaopai render intent when an OpenClaw chat-completions turn receives a stack-chan JSON event envelope with `schema` set to `openclaw.stackchan.event.v1` and `render.target` set to `xiaopai`.

#### Scenario: Stack-chan user utterance requires Xiaopai rendering
- **WHEN** the current turn includes a user message containing valid JSON with `schema: "openclaw.stackchan.event.v1"`, an `event` object, and `render.target: "xiaopai"`
- **THEN** the system records a per-turn Xiaopai render context
- **THEN** the render context includes the envelope `device_id` when present
- **THEN** the render context includes `interrupt` from `render.interrupt` when present

#### Scenario: Non-Xiaopai message is ignored by render fallback
- **WHEN** the current turn does not contain a valid stack-chan envelope with `render.target: "xiaopai"`
- **THEN** the system does not require Xiaopai fallback rendering for that turn

### Requirement: Explicit speech rendering suppresses fallback
The system MUST observe successful `xiaopaiControl.execute` calls during a Xiaopai-render-required turn and treat only speech-capable command execution as satisfying the spoken response requirement.

#### Scenario: Speak command satisfies rendering
- **WHEN** a Xiaopai-render-required turn calls `xiaopaiControl.execute` with a valid `speak` command containing non-empty text
- **AND** the command result indicates the command was successfully queued
- **THEN** the system marks the turn as speech-rendered
- **THEN** the system does not run a fallback speak command for the final assistant text

#### Scenario: Sequence with speak step satisfies rendering
- **WHEN** a Xiaopai-render-required turn calls `xiaopaiControl.execute` with a valid `sequence` command containing at least one `speak` step with non-empty text
- **AND** the command result indicates the command was successfully queued
- **THEN** the system marks the turn as speech-rendered
- **THEN** the system does not run a fallback speak command for the final assistant text

#### Scenario: Action-only command does not satisfy spoken rendering
- **WHEN** a Xiaopai-render-required turn calls `xiaopaiControl.execute` only with `face`, `action`, `move`, `stop`, or a `sequence` without any `speak` step
- **THEN** the system does not mark the turn as speech-rendered

#### Scenario: Failed command does not satisfy rendering
- **WHEN** a Xiaopai-render-required turn calls `xiaopaiControl.execute` with a speech-capable command
- **AND** the command result is rejected or failed instead of successfully queued
- **THEN** the system does not mark the turn as speech-rendered

### Requirement: Final text fallback queues Xiaopai speech
The system MUST call `xiaopaiControl.execute` as a fallback when a Xiaopai-render-required turn produces user-facing final assistant text and no successful speech-capable Xiaopai command was queued by the agent.

#### Scenario: Fallback speaks final assistant text
- **WHEN** a Xiaopai-render-required turn reaches final assistant text
- **AND** the final assistant text is non-empty and user-facing
- **AND** no successful speech-capable `xiaopaiControl.execute` call was observed for the turn
- **THEN** the system calls `xiaopaiControl.execute` with a `sequence` command
- **THEN** the sequence includes a `speak` step containing the normalized final assistant text
- **THEN** the command includes the stack-chan envelope `device_id` when available
- **THEN** the command includes `interrupt` from the stack-chan envelope render options when available

#### Scenario: Fallback omits device id when unavailable
- **WHEN** a fallback command is required
- **AND** no device id is available in the stack-chan envelope or embedded event payload
- **THEN** the system calls `xiaopaiControl.execute` without a `device_id`
- **THEN** the plugin remains responsible for applying its configured default device id or stack-chan server fallback targeting

#### Scenario: Fallback respects speech validation limits
- **WHEN** a fallback command is required
- **AND** the final assistant text exceeds the maximum length accepted by `xiaopaiControl.execute`
- **THEN** the system normalizes the spoken text to a non-empty value that fits the plugin speech text limit before calling `xiaopaiControl.execute`

#### Scenario: No final text skips fallback
- **WHEN** a Xiaopai-render-required turn completes without non-empty user-facing final assistant text
- **THEN** the system does not call fallback `xiaopaiControl.execute`

### Requirement: Xiaopai render fallback is observable
The system SHALL expose internal diagnostics for Xiaopai render fallback decisions without requiring stack-chan to parse OpenClaw response text.

#### Scenario: Explicit render is reported
- **WHEN** a Xiaopai-render-required turn is satisfied by an agent-issued speech-capable `xiaopaiControl.execute` call
- **THEN** diagnostics record the turn outcome as explicitly rendered

#### Scenario: Fallback render is reported
- **WHEN** the system queues a fallback Xiaopai speech command
- **THEN** diagnostics record the turn outcome as fallback rendered
- **THEN** diagnostics include the stack-chan event id when available

#### Scenario: Fallback failure is reported
- **WHEN** the system attempts fallback `xiaopaiControl.execute`
- **AND** the command result is rejected or failed
- **THEN** diagnostics record the turn outcome as fallback failed
- **THEN** diagnostics include the plugin result or sanitized error details

#### Scenario: Fallback skip reason is reported
- **WHEN** the system skips fallback for a turn
- **THEN** diagnostics record a skip reason such as `not_stackchan`, `not_xiaopai_target`, `no_final_text`, or `already_rendered`
