## ADDED Requirements

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
