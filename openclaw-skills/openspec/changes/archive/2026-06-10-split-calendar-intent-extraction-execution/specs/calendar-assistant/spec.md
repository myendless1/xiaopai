## ADDED Requirements

### Requirement: Structured calendar intent execution path
The calendar assistant SHALL prefer structured calendar creation intent input over natural-language parsing when both are present.

#### Scenario: Structured intent bypasses text parser
- **WHEN** a `user_utterance` event includes a valid `payload.structured_intent` with `type` set to `calendar.create`
- **THEN** the calendar assistant creates its internal calendar creation request from the structured intent
- **THEN** the calendar assistant does not require the original user utterance to match any wording-specific parser pattern

#### Scenario: Text fallback remains available
- **WHEN** a `user_utterance` event does not include `payload.structured_intent`
- **THEN** the calendar assistant may use the existing natural-language text parser
- **THEN** previously supported calendar creation utterances continue to work

### Requirement: Calendar creation from structured intent
The calendar assistant SHALL create a Lark calendar event from a validated structured calendar creation intent.

#### Scenario: Valid structured calendar request creates event
- **WHEN** the structured intent contains a title, valid start and end timestamps, requester identity, and uniquely resolved attendees
- **THEN** the system calls the Lark calendar adapter to create the event and invite the attendees
- **THEN** the response includes a successful `lark.calendar.create` action
- **THEN** the response includes `context_patch.last_created_calendar_event_id`

#### Scenario: Structured request with attendee ambiguity asks follow-up
- **WHEN** the structured intent includes an attendee name that resolves to multiple possible Lark users
- **THEN** the response asks the user to choose the intended attendee
- **THEN** the system does not create a Lark calendar event

#### Scenario: Structured request with missing attendee asks follow-up
- **WHEN** the structured intent includes an attendee name that cannot be resolved to a Lark user identifier
- **THEN** the response asks the user to correct or clarify that attendee
- **THEN** the system does not create a Lark calendar event

### Requirement: OpenClaw extraction guidance
The companion guidance SHALL instruct OpenClaw to perform structured extraction before calling the plugin for calendar creation.

#### Scenario: Flexible wording is handled before plugin execution
- **WHEN** the user says "明天上午10点到10点半有个活动 OpenClaw 测试，帮我建一个飞书日程，邀请 Gargantua 参会"
- **THEN** OpenClaw guidance produces a `calendar.create` structured intent with title "OpenClaw 测试", the resolved start and end timestamps, and attendee "Gargantua"
- **THEN** the plugin receives the structured intent and can execute without depending on the exact Chinese wording
