## Why

The workplace assistant now has calendar creation, agenda briefing, and wellbeing event handling, but proactive calendar-based behavior still lacks a deterministic trigger source. Polling the calendar through an LLM every few minutes would be costly, noisy, and hard to test; the system needs a scheduler that reads calendar data with adapters, builds trigger plans, and dispatches normalized events only when rules fire.

## What Changes

- Add a proactive calendar trigger scheduler that scans bounded future calendar windows and creates deterministic trigger plans.
- Dispatch normalized `InputEvent` objects for configured calendar-derived trigger types such as `daily_briefing_triggered`, `meeting_starting_soon`, `outdoor_event_detected`, and `business_trip_tomorrow_detected`.
- Add trigger state and idempotency behavior so repeated scans or restarts do not dispatch duplicate actionable events for the same calendar event and rule.
- Add configurable scan interval, lookahead window, enabled rules, reminder offsets, and dry-run behavior.
- Reuse the existing `LarkCalendarAdapter.listEvents` path and deterministic event classification patterns; do not add LLM-backed calendar polling.
- Keep meeting notification, Lark IM sending, route/weather lookup, and travel planning domain responses out of this change.

## Capabilities

### New Capabilities

- `proactive-calendar-trigger-scheduler`: Calendar read, trigger-plan generation, trigger deduplication, and dispatch of normalized assistant events without high-frequency LLM polling.

### Modified Capabilities

- `work-assistant-event-contract`: Add requirements for scheduler-produced system event metadata and side-effect-safe dispatch idempotency across proactive triggers.

## Impact

- Affected code: `plugins/work-assistant/src` scheduler module, plugin factory wiring/config, calendar dry-run fixtures, tests, README, packaged skill guidance, and verification notes.
- APIs: preserves the existing `workAssistant.handleEvent` execution boundary; may add an internal scheduler API and optional config, but no new public Gateway method is required for domain handling.
- Dependencies: reuses existing calendar adapter infrastructure; no model dependency, IM adapter, route adapter, weather adapter, or persistent database is required for the first implementation.
- Systems: OpenClaw/plugin runtime becomes responsible for creating proactive calendar trigger events; robot clients continue to consume `StructuredResponse` output from normal handler dispatch.
