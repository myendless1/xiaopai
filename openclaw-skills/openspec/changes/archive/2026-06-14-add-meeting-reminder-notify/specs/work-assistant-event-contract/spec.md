## ADDED Requirements

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

### Requirement: Lark message idempotency
The work assistant event contract SHALL treat successful Lark IM sends as side-effecting operations protected by `InputEvent.event_id`.

#### Scenario: Duplicate message-send event is idempotent
- **WHEN** the same successful meeting notification `InputEvent.event_id` is submitted more than once
- **THEN** the system does not send more than one Lark IM message for that event id
- **THEN** duplicate submissions return the original or an equivalent structured response when the idempotency record is available
