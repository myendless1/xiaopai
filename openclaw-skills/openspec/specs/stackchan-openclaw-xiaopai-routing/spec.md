# stackchan-openclaw-xiaopai-routing

## Purpose
Defines structured stack-chan event forwarding into OpenClaw and Xiaopai rendering ownership.

## Requirements

### Requirement: Stack-chan reports OpenClaw events as JSON envelopes
Stack-chan SHALL send OpenClaw-forwarded robot events as JSON envelopes with schema `openclaw.stackchan.event.v1` instead of prose event descriptions.

#### Scenario: Speech recognition is reported as user utterance
- **WHEN** stack-chan receives a non-empty speech recognition result that is eligible for OpenClaw forwarding
- **THEN** the OpenClaw chat-completions user message contains JSON with `schema` set to `openclaw.stackchan.event.v1`
- **THEN** the JSON contains `event.type` set to `user_utterance`
- **THEN** the JSON contains the recognized text in `event.payload.text`
- **THEN** the JSON contains `device_id`, `event.event_id`, `event.timestamp`, `event.user_id`, and `event.context.timezone`

#### Scenario: Non-touch device event preserves structured event details
- **WHEN** stack-chan receives a non-touch device event that is eligible for OpenClaw forwarding
- **THEN** the OpenClaw chat-completions user message contains JSON with `schema` set to `openclaw.stackchan.event.v1`
- **THEN** the JSON contains a work-assistant-compatible `event` object
- **THEN** original device event fields such as event name, source event type, and device id are preserved in the envelope or `event.payload`

### Requirement: Forwarded stack-chan events do not queue local presentation commands
Stack-chan MUST NOT queue Xiaopai speech, face, action, or motion commands for the same robot event that it delegates to OpenClaw.

#### Scenario: Touch event uses local face shortcut
- **WHEN** stack-chan receives a `head_touch` or `touch` event
- **THEN** stack-chan enqueues a local `face` command with expression `shy`
- **THEN** stack-chan does not send the touch event to OpenClaw
- **THEN** the HTTP response reports `openclaw_sent: false` and includes the local command id in `queued_commands`

#### Scenario: Speech event forwarding leaves command queue ownership to OpenClaw
- **WHEN** stack-chan forwards a speech recognition event to OpenClaw
- **THEN** stack-chan does not parse the OpenClaw text response for commands
- **THEN** stack-chan does not enqueue presentation commands from the upload handler itself
- **THEN** Xiaopai presentation for the event is produced only by later command API calls such as those made through `xiaopaiControl.execute`

### Requirement: OpenClaw agent conditionally routes stack-chan events
The companion guidance SHALL instruct OpenClaw agents to treat stack-chan JSON envelopes as structured robot input, then route only work-assistant-supported business events through `workAssistant.handleEvent` using the embedded `InputEvent`.

#### Scenario: Agent receives work-assistant-supported stack-chan event
- **WHEN** an OpenClaw agent receives a user message containing `schema: "openclaw.stackchan.event.v1"` and an `event` object
- **AND** the embedded event matches a work-assistant capability such as calendar creation, explicit agenda briefing, meeting reminder, late-arrival notification, sedentary-care, or wellbeing follow-up
- **THEN** the work-assistant companion guidance instructs the agent to call `workAssistant.handleEvent`
- **THEN** the Gateway call uses `{ "event": <envelope.event> }` as the method params
- **THEN** the agent treats the returned value as the canonical `StructuredResponse` for that business-handled robot turn

#### Scenario: Agent handles non-business stack-chan event directly
- **WHEN** an OpenClaw agent receives a user message containing `schema: "openclaw.stackchan.event.v1"` and an `event` object
- **AND** the event is ordinary chat, simple Q&A, a direct robot expression request, or a presentation-only command outside work-assistant's business capabilities
- **THEN** the companion guidance allows the agent to handle the event directly without calling `workAssistant.handleEvent`
- **THEN** any robot speech or presentation is still rendered through `xiaopaiControl.execute`

#### Scenario: Agent preserves stack-chan context for follow-up turns
- **WHEN** `workAssistant.handleEvent` returns a `StructuredResponse` with a non-empty `context_patch`
- **THEN** the companion guidance instructs the agent to preserve that context for later turns in the same device session when possible
- **THEN** subsequent stack-chan events can include relevant prior context in the `InputEvent.context`

### Requirement: OpenClaw agent renders robot responses through xiaopai-control
The companion guidance SHALL instruct OpenClaw agents to render stack-chan-triggered robot responses by calling `xiaopaiControl.execute`, whether the response came from `workAssistant.handleEvent` or was generated directly by the agent.

#### Scenario: Structured response with speech is rendered as sequence
- **WHEN** `workAssistant.handleEvent` returns a `StructuredResponse` with non-empty `speech`, or the agent directly produces an equivalent robot response with speech
- **THEN** the xiaopai-control companion guidance instructs the agent to call `xiaopaiControl.execute`
- **THEN** the command is preferably a `sequence` containing a `speak` step with the response speech
- **THEN** the command includes `device_id` from the stack-chan envelope when available
- **THEN** the command includes `interrupt` from the stack-chan envelope render options when available

#### Scenario: Supported presentation hints are rendered safely
- **WHEN** the `StructuredResponse.presentation` contains emotion or motion hints that map to Xiaopai-supported values
- **THEN** the xiaopai-control companion guidance instructs the agent to include corresponding `face`, `action`, or `move` steps in the sequence
- **THEN** the generated command uses only values accepted by `xiaopaiControl.execute`

#### Scenario: Unsupported presentation hints are omitted
- **WHEN** the `StructuredResponse.presentation` contains unsupported or unknown hints
- **THEN** the xiaopai-control companion guidance instructs the agent to omit those unsupported steps or use a documented safe mapping
- **THEN** the agent does not send unsupported expression, action, or motion values to `xiaopaiControl.execute`
