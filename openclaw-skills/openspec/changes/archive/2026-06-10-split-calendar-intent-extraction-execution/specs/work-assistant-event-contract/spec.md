## ADDED Requirements

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
