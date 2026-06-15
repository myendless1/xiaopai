## Context

The `work-assistant` plugin currently handles normalized events through `workAssistant.handleEvent`. It can create calendar events, generate agenda briefings, and respond to robot/sensing wellbeing events. The remaining proactive scenarios in the product direction need a reliable source of system events such as `meeting_starting_soon`, `outdoor_event_detected`, and `business_trip_tomorrow_detected`.

The important constraint is that proactive behavior must not be implemented as frequent LLM-driven calendar polling. Calendar state can be read deterministically through the existing `LarkCalendarAdapter.listEvents` path, and flexible model reasoning should remain at the OpenClaw intent-normalization layer or be used only for one-time extraction with cached results in later changes.

The scheduler is a cross-cutting infrastructure capability: it reads calendar data, creates trigger plans, deduplicates them, and dispatches normalized `InputEvent` objects to the existing handler boundary. It does not decide meeting-notification copy, send IM messages, compute routes, or query weather.

```text
startup / periodic refresh / calendar-change hint
        │
        ▼
bounded calendar scan
        │
        ▼
deterministic trigger rules
        │
        ▼
trigger plan store
        │
        ▼
due trigger dispatch
        │
        ▼
workAssistant.handleEvent-compatible handler
```

## Goals / Non-Goals

**Goals:**

- Add a `ProactiveCalendarTriggerScheduler` module that can run scan and dispatch ticks deterministically.
- Reuse `LarkCalendarAdapter.listEvents` for calendar reads and the existing `WorkAssistantHandler` shape for dispatch.
- Generate stable trigger plans for configured rules, including daily briefing, meeting-starting-soon, outdoor-event, and business-trip-tomorrow rules.
- Use stable trigger keys and stored dispatch state to avoid duplicate dispatch from repeated scans and process restarts.
- Provide dry-run fixtures and tests that prove proactive behavior without real Lark side effects.
- Add plugin config for enabling the scheduler, scan cadence, lookahead window, timezone, target user, calendar id, and rule settings.
- Keep LLM calls out of scheduler scans and trigger decisions.

**Non-Goals:**

- Do not implement `MeetingReminderAssistant`, `TravelPlannerAssistant`, Lark IM notification, route lookup, weather lookup, or user profile lookup.
- Do not add a new public Gateway method for executing domain behavior.
- Do not rely on parsing free-form text inside the scheduler.
- Do not require calendar webhooks in the first implementation, though the design leaves a refresh hook for them.
- Do not make every future trigger rule enabled by default before its consumer handler exists.

## Decisions

### Implement the scheduler as an orchestration module

Create a scheduler module with explicit methods such as `refresh(now)`, `dispatchDue(now)`, and `tick(now)`. The plugin runtime can optionally start an interval loop, while tests can call the methods directly.

Alternative considered: put trigger logic inside each domain assistant. Rejected because meeting reminders, outdoor reminders, and trip reminders share scanning, deduplication, and timer behavior. Duplicating that logic would make proactive behavior inconsistent.

### Use bounded calendar scans plus local trigger plans

The scheduler will read a bounded window, for example `now` through `now + 48h`, at startup and then on a configurable interval such as 10 or 15 minutes. It will also expose a refresh entry point that a future calendar-change integration can call.

This means the system queries calendar APIs periodically, but it does not ask an LLM to inspect the calendar on every interval. After plans are created, due dispatch is driven by local time and the trigger store.

Alternative considered: poll every two minutes and ask a model what to do. Rejected because it is expensive, harder to validate, and unnecessary for deterministic calendar triggers.

### Store trigger plans and dispatch records by stable keys

Each plan will have a stable key derived from `user_id`, `calendar_id`, `calendar_event_id` when present, `rule_id`, and `scheduled_for`. The stored record will also include an event hash built from the relevant calendar event fields and rule configuration.

Repeated scans upsert the same plan instead of creating duplicates. If the source calendar event moves, the event hash and scheduled time change, and the store updates the pending plan. When a trigger is dispatched successfully, the store records `dispatched_at` and the deterministic `event_id`.

The first implementation can provide a memory store for tests and a small JSON-file-backed store for runtime persistence. The store interface keeps a future SQLite or OpenClaw-native storage adapter possible without changing scheduler rules.

Alternative considered: rely only on `InputEvent.event_id` idempotency in the handler. Rejected because the scheduler must avoid noisy repeated speech or unsupported-event calls even before a domain handler performs side effects.

### Dispatch normalized events through the existing handler boundary

The scheduler will construct normalized `InputEvent` objects and call a provided dispatch function compatible with `WorkAssistantHandler.handleEvent`. It will not call domain assistants directly.

The event payload will include trigger metadata and, when the trigger comes from a calendar item, a normalized calendar event summary. The scheduler should preserve the existing `StructuredResponse` contract by recording dispatch outcomes internally, while the domain handler remains responsible for the returned speech and actions.

Alternative considered: add a new public Gateway method such as `workAssistant.runScheduler`. Rejected for domain behavior because the existing handler boundary is already transport-independent. A private test helper or internal exported scheduler class is enough.

### Keep unsupported future rules disabled by default

The scheduler can define rules for `meeting_starting_soon`, `outdoor_event_detected`, and `business_trip_tomorrow_detected`, but only rules whose consumer behavior is safe in the current plugin should be enabled by default. `daily_briefing_triggered` can be enabled because the agenda briefing handler already exists. Future changes can enable meeting and travel rules when their handlers are implemented.

Alternative considered: dispatch all planned future events immediately and let unsupported handlers reject them. Rejected because proactive infrastructure should not produce routine unsupported-event noise in production.

### Use deterministic classification for calendar-derived rules

Calendar-derived rules will use normalized event fields and deterministic configuration:

- meeting-starting-soon: events with a start time and meeting-like category or attendee/location indicators.
- outdoor-event: events matching configured outdoor/customer/site keywords or explicit metadata.
- business-trip-tomorrow: all-day or next-day travel-like events matching configured trip keywords.

The scheduler may reuse existing agenda classification helpers where practical, but it should keep rule-specific matching behind small pure functions so later changes can replace keyword classification with cached structured metadata.

Alternative considered: call a model during every scan to classify calendar events. Rejected because this would reintroduce high-frequency model polling.

## Risks / Trade-offs

- Calendar API reads can fail -> Return a failed scan result, keep existing pending plans, and avoid dispatching newly inferred triggers from incomplete data.
- Inaccurate keyword matching can schedule weak triggers -> Keep non-agenda future rules disabled by default and expose rule config; later changes can add cached structured extraction.
- Process restarts can duplicate reminders if state is not persisted -> Provide a persistent store option and tests for reloading dispatched records.
- Calendar events can move after plans are created -> Compare event hashes and scheduled times during refresh; update pending plans and avoid dispatching stale plans.
- Timezone bugs can fire at the wrong local time -> Derive local windows from `context.timezone` or scheduler config and test DST/offset-sensitive cases with fixed timestamps.
- The runtime interval may keep a process alive or complicate tests -> Make the loop optional and expose manual `tick` methods for deterministic tests.

## Migration Plan

1. Add scheduler contracts, config parsing, rule types, and trigger store interfaces without changing existing assistant behavior.
2. Implement calendar scanning, trigger plan generation, persistent dispatch state, and due dispatch through a handler callback.
3. Wire the scheduler into plugin startup only when `scheduler.enabled` is true; keep defaults conservative.
4. Add dry-run calendar fixtures, unit tests for rules/state/time windows, and smoke tests for enabled daily briefing dispatch.
5. Update README, packaged skill guidance, and verification notes with scheduler configuration and dry-run examples.
6. Validate with `npm run verify`, `openspec validate add-proactive-calendar-trigger-scheduler --strict`, and dry-run scheduler tick checks.

Rollback is to disable `scheduler.enabled` or remove the scheduler wiring. Existing manual `workAssistant.handleEvent` calls continue to work because domain handlers remain isolated.

## Open Questions

- Which persistence location should the runtime JSON trigger store use by default if OpenClaw does not provide a plugin state directory?
- Should the scheduler expose operational diagnostics through logs only, or through an internal debug method later?
- When meeting and travel handlers are added, should their rules become enabled by default or remain opt-in per deployment?
