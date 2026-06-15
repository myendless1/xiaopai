## ADDED Requirements

### Requirement: Normalized assistant event input
The system SHALL accept workplace assistant requests as normalized `InputEvent` objects containing `event_id`, `type`, `timestamp`, `user_id`, `payload`, and `context`.

#### Scenario: User utterance event is accepted
- **WHEN** the plugin receives an `InputEvent` with `type` set to `user_utterance`, a string `payload.text`, an ISO timestamp, and a timezone in `context.timezone`
- **THEN** the system routes the event to the assistant handler for intent processing

#### Scenario: Unsupported event type is rejected without side effects
- **WHEN** the plugin receives an `InputEvent` with an event type not supported by the MVP
- **THEN** the system returns a structured response explaining that the event type is unsupported
- **THEN** the system records no side-effecting action

### Requirement: Structured assistant response output
The system SHALL return a `StructuredResponse` for every accepted assistant event, including `speech`, `presentation`, `actions`, `follow_up`, and `context_patch` fields.

#### Scenario: Successful response contains robot presentation hints
- **WHEN** an assistant event is handled successfully
- **THEN** the response includes speech text for voice playback
- **THEN** the response includes presentation hints such as emotion, motion, or light values without directly controlling robot hardware

#### Scenario: Follow-up response asks for missing information
- **WHEN** the assistant cannot safely complete the requested operation
- **THEN** the response sets `follow_up.expected` to true
- **THEN** the response includes a concise follow-up question in `follow_up.question`

### Requirement: Action records describe side effects
The system SHALL describe every attempted external side effect in `actions` using a stable action type, status, and either resource identifiers or error details.

#### Scenario: Calendar event creation is reported
- **WHEN** the calendar assistant creates a Lark calendar event
- **THEN** the response includes an action with type `lark.calendar.create`
- **THEN** the action status is `success`
- **THEN** the action includes the created event resource identifier when available

#### Scenario: Adapter failure is reported
- **WHEN** an external adapter call fails after being attempted
- **THEN** the response includes an action with status `failed`
- **THEN** the response includes an error code or message suitable for troubleshooting without exposing secrets

### Requirement: Event idempotency for side effects
The system SHALL treat `event_id` as an idempotency key for side-effecting assistant operations.

#### Scenario: Duplicate event does not create duplicate calendar entries
- **WHEN** the same side-effecting `InputEvent.event_id` is submitted more than once
- **THEN** the system does not create more than one Lark calendar event for that event id
- **THEN** the system returns the original or equivalent structured response

### Requirement: Transport-independent assistant handling
The system SHALL keep assistant event handling independent from the transport used to submit the event.

#### Scenario: Gateway method uses shared handler
- **WHEN** a caller submits an assistant event through the plugin Gateway method
- **THEN** the system invokes the same domain handler that would be used by any future HTTP route wrapper
- **THEN** the returned payload conforms to the `StructuredResponse` contract
