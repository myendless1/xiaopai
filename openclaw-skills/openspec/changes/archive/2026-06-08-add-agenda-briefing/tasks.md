## 1. Contracts and Routing

- [x] 1.1 Add agenda briefing event type handling for `head_touch` and `daily_briefing_triggered` while preserving existing `InputEvent` validation.
- [x] 1.2 Update `WorkAssistantHandlerOptions` so the handler can receive an `AgendaBriefingAssistant` alongside `CalendarAssistant`.
- [x] 1.3 Route `user_utterance` events to the existing calendar assistant and agenda briefing trigger events to the new agenda assistant.
- [x] 1.4 Keep unsupported event handling and calendar creation idempotency behavior unchanged.

## 2. Lark Calendar Query Adapter

- [x] 2.1 Add calendar list request/result types and normalized agenda event types to the Lark adapter boundary.
- [x] 2.2 Implement `LarkCliCalendarAdapter.listEvents` using `lark-cli calendar +agenda --start --end --calendar-id --as --format json`.
- [x] 2.3 Normalize supported `lark-cli` calendar agenda JSON shapes into stable event summaries.
- [x] 2.4 Extend `DryRunCalendarAdapter` with deterministic agenda data for today's agenda and recap windows.
- [x] 2.5 Add adapter tests for successful list parsing, empty results, CLI failure, and malformed JSON failure.

## 3. Agenda Briefing Domain

- [x] 3.1 Add an `AgendaBriefingAssistant` module with `handle(event)` returning `StructuredResponse`.
- [x] 3.2 Implement local-day agenda window calculation from `event.timestamp` and `context.timezone`.
- [x] 3.3 Implement previous Monday-through-Friday recap window calculation.
- [x] 3.4 Implement deterministic event classification for `internal_meeting`, `customer_reception`, `outdoor_activity`, `deep_work`, and `uncategorized`.
- [x] 3.5 Implement recap totals and per-category count generation.
- [x] 3.6 Implement today's agenda highlight selection with a bounded number of spoken highlights.
- [x] 3.7 Implement template-based speech and presentation hints for normal, empty-agenda, empty-recap, and degraded-query responses.
- [x] 3.8 Populate `actions` with `lark.calendar.list` and `agenda.summary.generate` records and avoid write-side actions.
- [x] 3.9 Populate `context_patch` with briefing date, today's event count, highlight identifiers or titles, and category counts.

## 4. Tests

- [x] 4.1 Add handler tests proving `head_touch` and `daily_briefing_triggered` route to agenda briefing.
- [x] 4.2 Add tests proving `user_utterance` calendar creation behavior still routes to `CalendarAssistant`.
- [x] 4.3 Add agenda briefing tests for normal calendars, empty today agenda, empty recap window, query failure, and ordered highlights.
- [x] 4.4 Add classifier tests for each category and the `uncategorized` fallback.
- [x] 4.5 Add window calculation tests covering timestamp and timezone behavior.
- [x] 4.6 Add contract tests validating every agenda briefing response conforms to `StructuredResponse`.

## 5. Documentation and Skill Guidance

- [x] 5.1 Update `README.md` with agenda briefing event examples and response shape.
- [x] 5.2 Update required Lark scopes and runtime notes for calendar read/list access.
- [x] 5.3 Update packaged `skills/work-assistant/SKILL.md` guidance for `head_touch` and `daily_briefing_triggered` events.
- [x] 5.4 Add or update fixtures for dry-run agenda briefing Gateway invocation.

## 6. Verification

- [x] 6.1 Run `npm run verify` in `plugins/work-assistant`.
- [x] 6.2 Run `openspec validate add-agenda-briefing --strict`.
- [x] 6.3 Verify dry-run Gateway invocation for `head_touch`.
- [x] 6.4 Verify dry-run Gateway invocation for `daily_briefing_triggered`.
- [x] 6.5 Confirm existing calendar creation fixture and structured-intent tests still pass.
