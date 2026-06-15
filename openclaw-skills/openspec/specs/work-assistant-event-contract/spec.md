# work-assistant-event-contract

## Purpose
Defines the shared assistant event input, structured response output, action reporting, idempotency, and transport-independent handler contract.

## Requirements

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

### Requirement: Meeting reminder event routing
The work assistant event contract SHALL support `meeting_starting_soon` as a handled scheduler-produced event type.

#### Scenario: Meeting reminder event is accepted and routed
- **WHEN** `workAssistant.handleEvent` receives a valid `InputEvent` with `type` set to `meeting_starting_soon`
- **THEN** the system routes the event to the meeting reminder handler rather than returning an unsupported-event response
- **THEN** the returned payload conforms to the existing `StructuredResponse` contract

#### Scenario: Existing unsupported event behavior remains unchanged
- **WHEN** `workAssistant.handleEvent` receives an event type that is not supported by any work assistant domain handler
- **THEN** the system returns a structured unsupported-event response
- **THEN** the system records no side-effecting action

### Requirement: Structured intent payload support
The system SHALL allow `InputEvent.payload` to include a structured assistant intent object for deterministic execution.

#### Scenario: Gateway method accepts structured intent payload
- **WHEN** `workAssistant.handleEvent` receives an `InputEvent` whose `payload.structured_intent` contains a supported structured intent
- **THEN** the system routes the event to the matching assistant handler
- **THEN** the returned payload conforms to the existing `StructuredResponse` contract

#### Scenario: Original utterance text can accompany structured intent
- **WHEN** `payload.structured_intent` is present and `payload.text` also contains the original user utterance
- **THEN** the system preserves the ability to use `payload.text` for audit or debugging context
- **THEN** the system does not depend on `payload.text` for required structured fields already present in the structured intent

### Requirement: Meeting notification structured intent
The work assistant event contract SHALL allow `InputEvent.payload.structured_intent` to carry a deterministic meeting notification intent.

#### Scenario: Meeting notification structured intent is accepted
- **WHEN** `workAssistant.handleEvent` receives a `user_utterance` event whose `payload.structured_intent` has `type` set to `meeting.notify_late` and `version` set to `1`
- **THEN** the system may route the event to the meeting reminder handler for deterministic execution
- **THEN** the original `payload.text` remains optional audit or debugging context rather than the source of required structured fields

#### Scenario: Unsupported meeting notification intent version asks for clarification
- **WHEN** a meeting notification structured intent uses an unsupported `version`
- **THEN** the system returns a `StructuredResponse` with `follow_up.expected` set to true
- **THEN** the response records no Lark IM side effect

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

### Requirement: Structured intent validation errors are structured responses
The system MUST return a `StructuredResponse` rather than throwing transport-level errors for invalid structured intent payloads.

#### Scenario: Malformed structured intent returns follow-up
- **WHEN** `payload.structured_intent` is present but is not a valid object for any supported assistant intent
- **THEN** the system returns `follow_up.expected` set to true
- **THEN** the response includes no side-effecting action records

#### Scenario: Unsupported structured intent type returns follow-up
- **WHEN** `payload.structured_intent.type` is not supported by the work assistant
- **THEN** the system returns `follow_up.expected` set to true
- **THEN** the response includes a reason indicating that the structured intent type is unsupported
- **THEN** the system records no external side effect

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

### Requirement: Lark message action reporting
The work assistant event contract SHALL report attempted Lark IM notification side effects as stable action records.

#### Scenario: Successful Lark message send is reported
- **WHEN** the meeting reminder handler sends a Lark IM notification
- **THEN** the response includes an action with type `lark.message.send`
- **THEN** the action status is `success`
- **THEN** the action includes the message resource identifier when available and safe recipient details in `details`

#### Scenario: Failed Lark message send is reported
- **WHEN** the Lark IM adapter fails while sending a meeting notification
- **THEN** the response includes an action with type `lark.message.send` and status `failed`
- **THEN** the action includes an error code or message suitable for troubleshooting without exposing secrets

### Requirement: Event idempotency for side effects
The system SHALL treat `event_id` as an idempotency key for side-effecting assistant operations.

#### Scenario: Duplicate event does not create duplicate calendar entries
- **WHEN** the same side-effecting `InputEvent.event_id` is submitted more than once
- **THEN** the system does not create more than one Lark calendar event for that event id
- **THEN** the system returns the original or equivalent structured response

### Requirement: Lark message idempotency
The work assistant event contract SHALL treat successful Lark IM sends as side-effecting operations protected by `InputEvent.event_id`.

#### Scenario: Duplicate message-send event is idempotent
- **WHEN** the same successful meeting notification `InputEvent.event_id` is submitted more than once
- **THEN** the system does not send more than one Lark IM message for that event id
- **THEN** duplicate submissions return the original or an equivalent structured response when the idempotency record is available

### Requirement: Scheduler-produced system event metadata
Scheduler-produced assistant events SHALL use the existing `InputEvent` envelope and include stable trigger metadata in the payload.

#### Scenario: Calendar-derived trigger event includes source event summary
- **WHEN** the proactive scheduler dispatches a calendar-derived event such as `meeting_starting_soon`, `outdoor_event_detected`, or `business_trip_tomorrow_detected`
- **THEN** the dispatched `InputEvent` includes a deterministic `event_id`, the trigger event `type`, an ISO `timestamp`, `user_id`, `payload.trigger`, `payload.calendar_event`, and `context.timezone`
- **THEN** `payload.calendar_event` includes the source calendar event id, title, start time, end time, and location or description when available

#### Scenario: Time-based trigger event includes trigger metadata
- **WHEN** the proactive scheduler dispatches a time-based event such as `daily_briefing_triggered`
- **THEN** the dispatched `InputEvent` includes a deterministic `event_id`, the trigger event `type`, an ISO `timestamp`, `user_id`, `payload.trigger`, and `context.timezone`
- **THEN** `payload.trigger` identifies the scheduler rule id, scheduled time, fired time, and trigger source

### Requirement: Deterministic event ids for proactive triggers
Proactive scheduler events SHALL use deterministic event ids derived from stable trigger state so repeated scans and dispatch retries address the same logical trigger.

#### Scenario: Same trigger produces same event id
- **WHEN** the scheduler evaluates the same user, calendar id, source calendar event id, rule id, and scheduled trigger time more than once
- **THEN** it derives the same `InputEvent.event_id`
- **THEN** downstream idempotency logic can treat duplicate submissions as the same logical event

#### Scenario: Changed trigger produces new event id
- **WHEN** the source calendar event or rule configuration changes the scheduled trigger time before dispatch
- **THEN** the scheduler derives a new trigger key and event id for the updated trigger
- **THEN** the stale pending trigger is not dispatched after the update

### Requirement: Proactive dispatch action safety
Side-effecting handlers invoked by proactive trigger events MUST remain protected by `InputEvent.event_id` idempotency.

#### Scenario: Duplicate proactive side-effect event is submitted
- **WHEN** the same proactive `InputEvent.event_id` is submitted more than once to a handler that performs an external side effect
- **THEN** the system does not perform that side effect more than once for the event id
- **THEN** the duplicate submission returns the original or an equivalent structured response when the idempotency record is available

### Requirement: Transport-independent assistant handling
The system SHALL keep assistant event handling independent from the transport used to submit the event.

#### Scenario: Gateway method uses shared handler
- **WHEN** a caller submits an assistant event through the plugin Gateway method
- **THEN** the system invokes the same domain handler that would be used by any future HTTP route wrapper
- **THEN** the returned payload conforms to the `StructuredResponse` contract
