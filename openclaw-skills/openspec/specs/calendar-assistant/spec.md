# calendar-assistant

## Purpose
Defines Lark calendar creation behavior, including utterance parsing, validation, attendee resolution, side-effect reporting, and context patches.

## Requirements

### Requirement: Calendar creation utterance parsing
The system SHALL parse supported calendar creation utterances into a calendar creation intent containing title, date, start time, end time, and attendee names.

#### Scenario: Chinese meeting creation utterance is parsed
- **WHEN** the user utterance is "明天上午10点到11点的项目会，帮我建一个飞书日程，邀请张三、李四参会。"
- **THEN** the system extracts the title "项目会"
- **THEN** the system extracts attendee names "张三" and "李四"
- **THEN** the system extracts a start time of 10:00 and an end time of 11:00 for the resolved date

#### Scenario: Unsupported utterance requests clarification
- **WHEN** the utterance cannot be parsed into a calendar creation intent with enough confidence
- **THEN** the system returns a follow-up response
- **THEN** the system does not attempt Lark calendar creation

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

### Requirement: Relative time resolution
The system SHALL resolve relative dates and times using `InputEvent.timestamp` and `context.timezone`, not host wall-clock time.

#### Scenario: Tomorrow resolves from event timestamp
- **WHEN** the event timestamp is `2026-06-05T10:00:00+08:00`, `context.timezone` is `Asia/Shanghai`, and the utterance says "明天上午10点到11点"
- **THEN** the system resolves the event start time to `2026-06-06T10:00:00+08:00`
- **THEN** the system resolves the event end time to `2026-06-06T11:00:00+08:00`

### Requirement: Validation before calendar side effects
The system MUST validate required calendar creation fields before calling the Lark calendar creation adapter.

#### Scenario: Missing time triggers follow-up
- **WHEN** the utterance contains a title and attendees but no resolvable start and end time
- **THEN** the system asks the user to provide the meeting time
- **THEN** the system does not create a Lark calendar event

#### Scenario: Invalid time range triggers follow-up
- **WHEN** the parsed end time is not after the parsed start time
- **THEN** the system asks the user to clarify the time range
- **THEN** the system does not create a Lark calendar event

### Requirement: Lark attendee resolution
The system SHALL resolve attendee names through a Lark contact adapter before creating a calendar event.

#### Scenario: Unique attendee matches are accepted
- **WHEN** every attendee name resolves to exactly one Lark user identifier
- **THEN** the system includes those user identifiers in the calendar creation request

#### Scenario: Ambiguous attendee match triggers follow-up
- **WHEN** an attendee name resolves to multiple possible Lark users
- **THEN** the system asks the user to choose the intended attendee
- **THEN** the system does not create a Lark calendar event

#### Scenario: Missing attendee match triggers follow-up
- **WHEN** an attendee name cannot be resolved to a Lark user identifier
- **THEN** the system asks the user to correct or clarify that attendee
- **THEN** the system does not create a Lark calendar event

### Requirement: Lark calendar event creation
The system SHALL create a Lark calendar event only after parsing, validation, and attendee resolution succeed.

#### Scenario: Valid request creates calendar event
- **WHEN** the calendar creation intent has a title, resolved start and end times, requester identity, and uniquely resolved attendees
- **THEN** the system calls the Lark calendar adapter to create the event and invite the attendees
- **THEN** the response speech confirms that the event was created
- **THEN** the response includes a successful `lark.calendar.create` action

#### Scenario: Calendar adapter failure returns failed action
- **WHEN** the Lark calendar adapter rejects the create request
- **THEN** the response includes a failed `lark.calendar.create` action
- **THEN** the response speech explains that the event could not be created

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

### Requirement: Calendar context patch
The system SHALL include calendar-related context patches after successful calendar operations.

#### Scenario: Created event id is stored in context patch
- **WHEN** the calendar assistant creates a Lark calendar event and receives an event identifier
- **THEN** the response includes `context_patch.last_created_calendar_event_id`
- **THEN** the response includes enough event summary data for a later follow-up capability to reference the created event

### Requirement: OpenClaw extraction guidance
The companion guidance SHALL instruct OpenClaw to perform structured extraction before calling the plugin for calendar creation.

#### Scenario: Flexible wording is handled before plugin execution
- **WHEN** the user says "明天上午10点到10点半有个活动 OpenClaw 测试，帮我建一个飞书日程，邀请 Gargantua 参会"
- **THEN** OpenClaw guidance produces a `calendar.create` structured intent with title "OpenClaw 测试", the resolved start and end timestamps, and attendee "Gargantua"
- **THEN** the plugin receives the structured intent and can execute without depending on the exact Chinese wording
