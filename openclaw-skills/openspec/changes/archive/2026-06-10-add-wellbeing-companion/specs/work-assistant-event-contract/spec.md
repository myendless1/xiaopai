## MODIFIED Requirements

### Requirement: Normalized assistant event input
The system SHALL accept workplace assistant requests as normalized `InputEvent` objects containing `event_id`, `type`, `timestamp`, `user_id`, `payload`, and `context`.

#### Scenario: User utterance event is accepted
- **WHEN** the plugin receives an `InputEvent` with `type` set to `user_utterance`, a string `payload.text`, an ISO timestamp, and a timezone in `context.timezone`
- **THEN** the system routes the event to the assistant handler for intent processing

#### Scenario: Sedentary detection event is accepted
- **WHEN** the plugin receives an `InputEvent` with `type` set to `sedentary_detected`, an ISO timestamp, and a timezone in `context.timezone`
- **THEN** the system routes the event to the wellbeing companion handler for sedentary-care processing

#### Scenario: Wellbeing follow-up event is accepted
- **WHEN** the plugin receives an `InputEvent` with `type` set to `wellbeing_companion_requested`, an ISO timestamp, and a timezone in `context.timezone`
- **THEN** the system routes the event to the wellbeing companion handler for light-companionship response generation

#### Scenario: Unsupported event type is rejected without side effects
- **WHEN** the plugin receives an `InputEvent` with an event type that is not supported by the work assistant
- **THEN** the system returns a structured response explaining that the event type is unsupported
- **THEN** the system records no side-effecting action

### Requirement: Action records describe side effects
The system SHALL describe every attempted external side effect and significant assistant operation in `actions` using a stable action type, status, and either resource identifiers, operation details, or error details.

#### Scenario: Calendar event creation is reported
- **WHEN** the calendar assistant creates a Lark calendar event
- **THEN** the response includes an action with type `lark.calendar.create`
- **THEN** the action status is `success`
- **THEN** the action includes the created event resource identifier when available

#### Scenario: Adapter failure is reported
- **WHEN** an external adapter call fails after being attempted
- **THEN** the response includes an action with status `failed`
- **THEN** the response includes an error code or message suitable for troubleshooting without exposing secrets

#### Scenario: Wellbeing advisory decision is reported
- **WHEN** the wellbeing companion evaluates whether to send or skip a sedentary nudge
- **THEN** the response includes an action with type `wellbeing.sedentary.evaluate`
- **THEN** the action details include the evaluation decision or skip reason
