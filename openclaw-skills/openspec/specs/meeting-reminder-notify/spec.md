# meeting-reminder-notify

## Purpose
Defines proactive meeting reminders and focused late-arrival notification follow-ups.

## Requirements

### Requirement: Proactive meeting reminder handling
The system SHALL handle scheduler-produced `meeting_starting_soon` events as proactive meeting reminders.

#### Scenario: Meeting reminder event produces a structured reminder
- **WHEN** `workAssistant.handleEvent` receives an `InputEvent` with `type` set to `meeting_starting_soon`, scheduler trigger metadata, and `payload.calendar_event`
- **THEN** the system routes the event to the meeting reminder assistant
- **THEN** the response includes concise speech naming the meeting title, start time, and location when available
- **THEN** the response includes a `meeting.reminder.generate` action with status `success`
- **THEN** the response does not send a Lark message as part of the reminder itself

#### Scenario: Malformed meeting reminder event is handled without side effects
- **WHEN** a `meeting_starting_soon` event is missing the source calendar event summary required to generate a reminder
- **THEN** the response includes a failed `meeting.reminder.generate` action
- **THEN** the response records no Lark IM side effect
- **THEN** the response does not ask the user for a follow-up caused by scheduler metadata defects

### Requirement: Meeting focus context
The system SHALL preserve the reminded meeting as short-lived focus context for immediate follow-up commands.

#### Scenario: Successful reminder stores current focus
- **WHEN** a meeting reminder is generated successfully
- **THEN** `context_patch.current_focus` includes the calendar event id, calendar id when available, title, start time, end time, and location when available
- **THEN** `context_patch.current_focus.type` is set to `calendar_event`
- **THEN** downstream user utterances can use that context to refer to the meeting implicitly

#### Scenario: Notification target is stored only when known
- **WHEN** scheduler or adapter data includes a meeting chat id or attendee user ids for the calendar event
- **THEN** `context_patch.current_focus.notification_target` includes those recipient identifiers
- **WHEN** no reliable notification target is available
- **THEN** the focus context omits `notification_target`

### Requirement: Late-arrival notification follow-up
The system SHALL support a narrow meeting notification follow-up that uses the current focused meeting.

#### Scenario: Structured late notification intent sends Lark message
- **WHEN** `workAssistant.handleEvent` receives a `user_utterance` with `payload.structured_intent.type` set to `meeting.notify_late`
- **AND** `context.current_focus` identifies a current calendar event with a usable notification target
- **THEN** the system sends one Lark IM message to the focused meeting target
- **THEN** the message content includes the configured or extracted delay in minutes when present
- **THEN** the response includes a successful `lark.message.send` action and confirmation speech

#### Scenario: Deterministic fallback handles clear late notification wording
- **WHEN** a `user_utterance` has current meeting focus and clearly says the user will be late and asks to notify meeting participants
- **THEN** the system may derive a `meeting.notify_late` request through deterministic parsing
- **THEN** the system uses a bounded notification template rather than open-ended text generation

#### Scenario: Missing focus asks for the meeting
- **WHEN** the user asks to notify meeting participants but `context.current_focus` does not identify a calendar event
- **THEN** the response sets `follow_up.expected` to true
- **THEN** the follow-up asks which meeting should be used
- **THEN** no Lark message is sent

### Requirement: Notification target safety
The system MUST avoid sending meeting notification messages when recipients are unavailable or ambiguous.

#### Scenario: Missing notification target asks for recipients
- **WHEN** a late notification request has focused meeting context but no chat id or attendee user ids
- **THEN** the response sets `follow_up.expected` to true
- **THEN** the response asks the user to specify who or which chat should receive the notice
- **THEN** no `lark.message.send` action with status `success` is recorded

#### Scenario: Lark IM adapter failure is reported
- **WHEN** the system attempts to send a meeting notification and the Lark IM adapter fails
- **THEN** the response includes a failed `lark.message.send` action with a safe error code or message
- **THEN** the response speech explains that the notification could not be sent
- **THEN** the focused meeting context remains available for retry

### Requirement: Meeting notification idempotency
The system SHALL treat the follow-up `InputEvent.event_id` as an idempotency key for Lark IM side effects.

#### Scenario: Duplicate notification event does not send duplicate messages
- **WHEN** the same successful late notification `InputEvent.event_id` is submitted more than once
- **THEN** the system sends at most one Lark IM message for that event id
- **THEN** duplicate submissions return the original or an equivalent structured response

### Requirement: Dry-run meeting reminder and notification behavior
The system SHALL support deterministic dry-run verification for meeting reminders and notifications.

#### Scenario: Dry-run reminder uses scheduler fixture data
- **WHEN** dry-run mode is enabled and a scheduler tick dispatches a due `meeting_starting_soon` trigger with the meeting rule explicitly enabled
- **THEN** the meeting reminder handler returns a stable structured response
- **THEN** no real Lark IM write is attempted during the reminder

#### Scenario: Dry-run notification records deterministic message result
- **WHEN** dry-run mode is enabled and a late notification follow-up has a usable dry-run notification target
- **THEN** the dry-run IM adapter returns a deterministic message id
- **THEN** the response records a successful `lark.message.send` action without calling `lark-cli`
