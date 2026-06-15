## ADDED Requirements

### Requirement: Agenda briefing trigger handling
The system SHALL route supported agenda briefing trigger events to an agenda briefing domain handler while preserving the existing `InputEvent` and `StructuredResponse` envelope.

#### Scenario: Head touch triggers agenda briefing
- **WHEN** `workAssistant.handleEvent` receives an `InputEvent` with `type` set to `head_touch`
- **THEN** the system invokes the agenda briefing handler
- **THEN** the system returns a `StructuredResponse` with `speech`, `presentation`, `actions`, `follow_up`, and `context_patch`

#### Scenario: Scheduled daily briefing triggers agenda briefing
- **WHEN** `workAssistant.handleEvent` receives an `InputEvent` with `type` set to `daily_briefing_triggered`
- **THEN** the system invokes the agenda briefing handler
- **THEN** the system returns a `StructuredResponse` with `follow_up.expected` set to false unless required event context is invalid

### Requirement: Agenda calendar query windows
The agenda briefing handler SHALL query calendar events for both today's agenda window and a recent recap window derived from the event timestamp and timezone.

#### Scenario: Today's agenda window is queried
- **WHEN** an agenda briefing event has a valid ISO `timestamp` and `context.timezone`
- **THEN** the system derives the start and end of the local calendar day containing that timestamp
- **THEN** the system queries the Lark calendar adapter for events in that local-day window

#### Scenario: Previous work-week recap window is queried
- **WHEN** an agenda briefing event has a valid ISO `timestamp` and `context.timezone`
- **THEN** the system derives the previous Monday-through-Friday recap window before the briefing date
- **THEN** the system queries the Lark calendar adapter for events in that recap window

### Requirement: Agenda event normalization
The system SHALL normalize Lark calendar query results into agenda event summaries containing stable fields needed for briefing generation.

#### Scenario: Calendar event fields are normalized
- **WHEN** the Lark calendar adapter returns calendar events
- **THEN** the system extracts an event identifier, title, start time, end time, location, description or remarks when available, and attendee count when available
- **THEN** events missing optional fields remain eligible for briefing generation

#### Scenario: Calendar events are ordered by start time
- **WHEN** multiple agenda events are available for today's agenda window
- **THEN** the system orders them by start time before selecting spoken highlights

### Requirement: Agenda event classification
The system SHALL classify calendar events using deterministic rules before generating recap counts and agenda highlights.

#### Scenario: Events are assigned briefing categories
- **WHEN** normalized calendar events are processed for briefing
- **THEN** each event is assigned one of `internal_meeting`, `customer_reception`, `outdoor_activity`, `deep_work`, or `uncategorized`
- **THEN** the category assignment is based on configurable deterministic rules using event title, location, and description or remarks

#### Scenario: Unknown events remain uncategorized
- **WHEN** an event does not match any configured category rule
- **THEN** the system assigns `uncategorized`
- **THEN** the system does not fail the briefing because the event could not be classified

### Requirement: Recent work recap generation
The agenda briefing handler SHALL generate a recent work recap from the recap window calendar events.

#### Scenario: Recap includes total and category counts
- **WHEN** recap window events are available
- **THEN** the response includes recap totals and per-category counts in generated briefing data
- **THEN** the spoken `speech` summarizes the recap without listing every recap event

#### Scenario: Empty recap window is handled
- **WHEN** no events are found in the recap window
- **THEN** the system generates a valid briefing response
- **THEN** the spoken `speech` states that there were no recent calendar items to recap or omits recap details without failing

### Requirement: Today's agenda highlight generation
The agenda briefing handler SHALL generate concise highlights for today's agenda, prioritizing important events over complete event enumeration.

#### Scenario: Today's agenda includes prioritized highlights
- **WHEN** today's agenda contains multiple events
- **THEN** the system selects a bounded number of highlights based on start time, category, location, and preparation indicators
- **THEN** the spoken `speech` includes the selected highlights rather than every event when the agenda is busy

#### Scenario: Empty today agenda is handled
- **WHEN** no events are found in today's agenda window
- **THEN** the system generates a valid briefing response
- **THEN** the spoken `speech` says the user has no calendar items scheduled for today or equivalent concise wording

### Requirement: Agenda briefing action reporting
The agenda briefing handler SHALL record read attempts and summary generation in `StructuredResponse.actions`.

#### Scenario: Successful briefing reports calendar list actions
- **WHEN** today's agenda and recap calendar queries both succeed
- **THEN** the response includes successful `lark.calendar.list` actions for the queried windows
- **THEN** the response includes a successful `agenda.summary.generate` action

#### Scenario: Calendar query failure is reported
- **WHEN** a Lark calendar query fails during agenda briefing
- **THEN** the response includes a failed `lark.calendar.list` action with an error code or message
- **THEN** the system returns a degraded `StructuredResponse` without attempting any write-side Lark action

### Requirement: Agenda briefing response context
The agenda briefing handler SHALL include short-term briefing context that later assistant capabilities can reuse.

#### Scenario: Briefing context patch includes agenda summary
- **WHEN** agenda briefing completes successfully
- **THEN** `context_patch` includes the briefing date, today's event count, selected highlight event identifiers or titles, and category counts
- **THEN** `follow_up.expected` is false
