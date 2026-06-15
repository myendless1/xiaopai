# proactive-calendar-trigger-scheduler

## Purpose
Defines deterministic proactive calendar scanning, trigger planning, deduplication, and dispatch behavior for Work Assistant calendar-derived system events.

## Requirements

### Requirement: Scheduler activation and configuration
The system SHALL provide a proactive calendar trigger scheduler that is disabled unless explicitly enabled through plugin configuration.

#### Scenario: Scheduler stays inactive by default
- **WHEN** the plugin is created without scheduler configuration
- **THEN** the scheduler does not start a periodic loop
- **THEN** existing `workAssistant.handleEvent` behavior remains unchanged

#### Scenario: Scheduler uses configured cadence and windows
- **WHEN** scheduler configuration enables proactive triggers with a scan interval, lookahead window, timezone, user id, calendar id, and enabled rules
- **THEN** the scheduler uses those configured values when calculating scan windows and trigger plans
- **THEN** invalid or missing optional values fall back to documented safe defaults

### Requirement: Bounded calendar scanning
The scheduler SHALL scan bounded calendar windows through `LarkCalendarAdapter.listEvents` and MUST NOT call an LLM or model during recurring calendar scans.

#### Scenario: Startup scan reads a future calendar window
- **WHEN** the scheduler is started at a valid timestamp
- **THEN** it queries calendar events from the configured scan start through the configured lookahead horizon
- **THEN** the scan records a `proactive.calendar.scan` result containing the queried window and event count

#### Scenario: Recurring scan avoids model polling
- **WHEN** a periodic scheduler refresh runs
- **THEN** the scheduler uses adapter results and deterministic rule functions to evaluate triggers
- **THEN** no model-backed calendar inspection is performed as part of the recurring scan

#### Scenario: Calendar scan failure is non-destructive
- **WHEN** `LarkCalendarAdapter.listEvents` fails during a refresh
- **THEN** the scheduler records a failed `proactive.calendar.scan` result with an error code or message
- **THEN** existing pending trigger plans are not deleted because of the failed scan

### Requirement: Deterministic trigger plan generation
The scheduler SHALL generate trigger plans from configured deterministic rules and normalized calendar event fields.

#### Scenario: Daily briefing plan is generated
- **WHEN** the `daily_briefing` rule is enabled with a configured local briefing time
- **THEN** the scheduler creates a trigger plan for an `InputEvent` with `type` set to `daily_briefing_triggered`
- **THEN** the plan is scheduled for the configured local briefing time in the scheduler timezone

#### Scenario: Meeting reminder plan is generated from calendar events
- **WHEN** the `meeting_starting_soon` rule is enabled and a calendar event matches the configured meeting criteria
- **THEN** the scheduler creates a trigger plan for an `InputEvent` with `type` set to `meeting_starting_soon`
- **THEN** the plan is scheduled at the configured reminder offset before the event start time

#### Scenario: Outdoor event plan is generated from calendar events
- **WHEN** the `outdoor_event` rule is enabled and a calendar event matches configured outdoor or customer-site criteria
- **THEN** the scheduler creates a trigger plan for an `InputEvent` with `type` set to `outdoor_event_detected`
- **THEN** the plan includes a normalized summary of the source calendar event

#### Scenario: Business trip plan is generated for next-day travel
- **WHEN** the `business_trip_tomorrow` rule is enabled and a calendar event on the next local day matches configured trip criteria
- **THEN** the scheduler creates a trigger plan for an `InputEvent` with `type` set to `business_trip_tomorrow_detected`
- **THEN** the plan is scheduled for the configured local pre-trip reminder time

### Requirement: Trigger plan storage and deduplication
The scheduler SHALL persist trigger plans and dispatch records using stable trigger keys so repeated scans do not create duplicate dispatches.

#### Scenario: Repeated scan upserts an existing plan
- **WHEN** two refreshes see the same calendar event, rule id, and scheduled trigger time
- **THEN** the scheduler stores one trigger plan using the same stable trigger key
- **THEN** it does not enqueue duplicate dispatches for that plan

#### Scenario: Moved calendar event updates a pending plan
- **WHEN** a calendar event that has a pending trigger plan changes its start time before dispatch
- **THEN** the scheduler updates the pending plan scheduled time and event hash
- **THEN** the stale scheduled time is not dispatched

#### Scenario: Dispatched plan is not dispatched again after state reload
- **WHEN** a trigger plan has been marked dispatched and the scheduler reloads persisted trigger state
- **THEN** the scheduler does not dispatch that same trigger key again
- **THEN** the stored dispatch record includes the deterministic event id and dispatch timestamp

### Requirement: Due trigger dispatch
The scheduler SHALL dispatch due trigger plans as normalized `InputEvent` objects through a provided handler-compatible dispatch function.

#### Scenario: Due trigger invokes the handler boundary
- **WHEN** a pending trigger plan has `scheduled_for` less than or equal to the scheduler tick time
- **THEN** the scheduler constructs a normalized `InputEvent` with a deterministic `event_id`
- **THEN** the scheduler calls the configured dispatch function with that event

#### Scenario: Successful dispatch marks the plan dispatched
- **WHEN** a due trigger dispatch returns a successful handler response
- **THEN** the scheduler marks the trigger plan as dispatched
- **THEN** later ticks do not dispatch the same trigger key again

#### Scenario: Dispatch failure remains retryable
- **WHEN** a due trigger dispatch throws or returns an unsuccessful dispatch result
- **THEN** the scheduler records the failure
- **THEN** the plan remains pending or retryable according to configured retry limits

### Requirement: Rule enablement protects unsupported consumers
The scheduler SHALL only dispatch trigger types whose rules are enabled in configuration.

#### Scenario: Future handler rule remains disabled
- **WHEN** `meeting_starting_soon`, `outdoor_event`, or `business_trip_tomorrow` rules are not enabled
- **THEN** the scheduler does not dispatch those trigger types even if matching calendar events exist
- **THEN** the scheduler may still report that matching events were observed for diagnostics

#### Scenario: Supported daily briefing rule can dispatch
- **WHEN** the `daily_briefing` rule is enabled and a due briefing plan exists
- **THEN** the scheduler dispatches a `daily_briefing_triggered` event through the existing handler boundary
- **THEN** the agenda briefing handler can generate the normal structured briefing response

### Requirement: Dry-run scheduler behavior
The scheduler SHALL support deterministic dry-run operation for local verification without real Lark writes or robot side effects.

#### Scenario: Dry-run scan uses deterministic calendar data
- **WHEN** plugin dry-run mode is enabled and the scheduler refreshes
- **THEN** the scheduler uses deterministic dry-run calendar events
- **THEN** generated trigger plans are stable across test runs with the same timestamp and configuration

#### Scenario: Dry-run dispatch records responses
- **WHEN** a dry-run scheduler tick dispatches a due trigger
- **THEN** the scheduler records the dispatched event id, trigger type, and structured response summary
- **THEN** no Lark write action is attempted unless a future enabled handler explicitly performs one
