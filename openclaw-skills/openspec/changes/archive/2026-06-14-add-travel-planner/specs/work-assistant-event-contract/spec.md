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

#### Scenario: Outdoor event trigger is accepted
- **WHEN** the plugin receives an `InputEvent` with `type` set to `outdoor_event_detected`, an ISO timestamp, a timezone in `context.timezone`, scheduler trigger metadata, and a calendar event summary when available
- **THEN** the system routes the event to the travel planner handler for outdoor visit guidance

#### Scenario: Business trip trigger is accepted
- **WHEN** the plugin receives an `InputEvent` with `type` set to `business_trip_tomorrow_detected`, an ISO timestamp, a timezone in `context.timezone`, scheduler trigger metadata, and a calendar event summary when available
- **THEN** the system routes the event to the travel planner handler for next-day trip guidance

#### Scenario: Unsupported event type is rejected without side effects
- **WHEN** the plugin receives an `InputEvent` with an event type that is not supported by the work assistant
- **THEN** the system returns a structured response explaining that the event type is unsupported
- **THEN** the system records no side-effecting action
