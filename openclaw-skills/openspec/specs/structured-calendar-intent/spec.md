# structured-calendar-intent

## Purpose
Defines the deterministic structured calendar intent shape used before the Work Assistant plugin executes calendar creation.

## Requirements

### Requirement: Structured calendar create intent shape
The system SHALL define a versioned structured intent shape for calendar creation requests.

#### Scenario: Complete structured calendar intent is accepted
- **WHEN** a structured intent has `type` set to `calendar.create`, `version` set to `1`, a non-empty `title`, ISO `start` and `end` timestamps, and at least one attendee
- **THEN** the system treats the structured intent as a candidate calendar creation request
- **THEN** the system does not require wording-specific title or time extraction from the original user text

#### Scenario: Unsupported structured intent version is rejected
- **WHEN** a structured intent has an unsupported `version`
- **THEN** the system returns a follow-up response with an unsupported intent version reason
- **THEN** the system does not create a Lark calendar event

### Requirement: Structured calendar intent validation
The system MUST validate structured calendar creation intent fields before any external side effect.

#### Scenario: Missing required structured field prevents side effects
- **WHEN** a structured calendar create intent is missing `title`, `start`, `end`, or attendees
- **THEN** the system returns a follow-up response identifying that required calendar information is missing
- **THEN** the system does not call the Lark contact adapter
- **THEN** the system does not call the Lark calendar adapter

#### Scenario: Invalid structured time range prevents side effects
- **WHEN** a structured calendar create intent has an `end` timestamp that is not after `start`
- **THEN** the system returns a follow-up response asking for a valid time range
- **THEN** the system does not create a Lark calendar event

### Requirement: Structured attendee references
The system SHALL support structured attendee references by name and by Lark attendee identifier.

#### Scenario: Structured attendee name is resolved
- **WHEN** a structured calendar create intent includes an attendee with `name`
- **THEN** the system resolves that name through the Lark contact adapter before creating the event
- **THEN** the system includes the resolved attendee identifier in the Lark calendar creation request

#### Scenario: Structured attendee identifier bypasses contact lookup
- **WHEN** a structured calendar create intent includes an attendee with a valid Lark attendee `id`
- **THEN** the system uses that identifier in the Lark calendar creation request
- **THEN** the system does not require contact lookup for that attendee

#### Scenario: Invalid structured attendee reference is rejected
- **WHEN** a structured attendee has neither a non-empty `name` nor a valid Lark attendee `id`
- **THEN** the system returns a follow-up response asking for a valid attendee
- **THEN** the system does not create a Lark calendar event
