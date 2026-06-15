## 1. Contracts and Configuration

- [x] 1.1 Add TypeScript types for scheduler config, trigger rules, trigger plans, trigger store records, scan results, and dispatch results.
- [x] 1.2 Extend plugin config parsing and `openclaw.plugin.json` with optional `scheduler` settings including enabled flag, scan interval, lookahead, timezone, user id, calendar id, state path, and rule settings.
- [x] 1.3 Add helper validation for scheduler config with safe defaults and disabled-by-default behavior.
- [x] 1.4 Add scheduler-produced payload types for `payload.trigger` and `payload.calendar_event` while preserving the existing `InputEvent` envelope.

## 2. Scheduler Core

- [x] 2.1 Create a `ProactiveCalendarTriggerScheduler` module with `refresh(now)`, `dispatchDue(now)`, and `tick(now)` methods.
- [x] 2.2 Implement bounded calendar scan window calculation from scheduler timestamp, timezone, and lookahead configuration.
- [x] 2.3 Call `LarkCalendarAdapter.listEvents` during refresh and record successful or failed `proactive.calendar.scan` results.
- [x] 2.4 Ensure recurring scan logic uses deterministic adapters and rule functions only, with no model or LLM calls.

## 3. Trigger Rules

- [x] 3.1 Implement the `daily_briefing` rule that schedules `daily_briefing_triggered` at a configured local time.
- [x] 3.2 Implement the `meeting_starting_soon` rule that schedules `meeting_starting_soon` at a configured offset before matching meeting events.
- [x] 3.3 Implement the `outdoor_event` rule that schedules `outdoor_event_detected` for matching outdoor or customer-site events.
- [x] 3.4 Implement the `business_trip_tomorrow` rule that schedules `business_trip_tomorrow_detected` at a configured pre-trip local time for next-day trip events.
- [x] 3.5 Keep unsupported future handler rules disabled by default while allowing tests and explicit config to enable them.

## 4. Trigger Store and Dispatch

- [x] 4.1 Implement stable trigger key and deterministic `InputEvent.event_id` derivation from user id, calendar id, source event id, rule id, and scheduled time.
- [x] 4.2 Implement an in-memory trigger store for unit tests.
- [x] 4.3 Implement a JSON-file-backed trigger store for runtime persistence, including reload of dispatched records after restart.
- [x] 4.4 Upsert repeated scan results into existing pending plans instead of creating duplicate dispatches.
- [x] 4.5 Update pending plans when a source calendar event changes before dispatch and suppress stale scheduled times.
- [x] 4.6 Dispatch due plans through a handler-compatible callback and mark plans dispatched only after successful dispatch.
- [x] 4.7 Record dispatch failures and leave plans pending or retryable according to configured retry limits.

## 5. Plugin Wiring and Dry Run

- [x] 5.1 Instantiate the scheduler in the plugin factory only when `scheduler.enabled` is true.
- [x] 5.2 Add optional interval loop startup and shutdown handling without changing manual `workAssistant.handleEvent` calls.
- [x] 5.3 Extend dry-run calendar data so scheduler scans can deterministically produce briefing, meeting, outdoor, and trip plans.
- [x] 5.4 Export scheduler classes and store implementations where useful for tests without exposing a new public domain Gateway method.

## 6. Tests

- [x] 6.1 Add unit tests for scheduler config parsing, safe defaults, and disabled-by-default behavior.
- [x] 6.2 Add unit tests for scan window calculation and calendar scan failure handling.
- [x] 6.3 Add unit tests for each trigger rule and the generated event payload shape.
- [x] 6.4 Add store tests proving repeated scans upsert plans, moved events update pending plans, and dispatched records survive state reload.
- [x] 6.5 Add dispatch tests proving due plans call the handler boundary, successful dispatch marks plans dispatched, and failed dispatch remains retryable.
- [x] 6.6 Add smoke tests for scheduler-enabled dry-run daily briefing dispatch through the existing agenda briefing handler.
- [x] 6.7 Add contract tests for deterministic proactive event ids and scheduler-produced metadata.

## 7. Documentation and Verification

- [x] 7.1 Update `plugins/work-assistant/README.md` with scheduler purpose, config examples, rule defaults, and the no-LLM-polling boundary.
- [x] 7.2 Update `plugins/work-assistant/skills/work-assistant/SKILL.md` to explain that proactive trigger events come from scheduler output rather than ad hoc LLM calendar polling.
- [x] 7.3 Add fixture examples for scheduler-produced `daily_briefing_triggered`, `meeting_starting_soon`, `outdoor_event_detected`, and `business_trip_tomorrow_detected` events.
- [x] 7.4 Run `npm run verify` in `plugins/work-assistant`.
- [x] 7.5 Run `openspec validate add-proactive-calendar-trigger-scheduler --strict`.
- [x] 7.6 Record dry-run scheduler tick verification results in `plugins/work-assistant/VERIFICATION.md`.
