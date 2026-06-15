## Context

The `work-assistant` plugin currently exposes one Gateway method, `workAssistant.handleEvent`, and routes valid `user_utterance` events to `CalendarAssistant`. Calendar creation now has a clearer boundary: OpenClaw extracts structured intent, and the plugin validates and executes deterministic Lark side effects.

Agenda briefing is the next capability in the design direction. It needs a broader event surface than calendar creation because it can be triggered by a robot event (`head_touch`) or a scheduled/system event (`daily_briefing_triggered`). It also introduces read-side Lark calendar access and calendar summarization, but should keep the same `InputEvent` and `StructuredResponse` envelope.

## Goals / Non-Goals

**Goals:**

- Add an `AgendaBriefingAssistant` domain handler for `head_touch` and `daily_briefing_triggered` events.
- Query Lark calendar events for today's agenda and a recent recap window.
- Classify calendar events into configurable categories: internal meetings, customer receptions, outdoor activities, deep work, and uncategorized.
- Generate concise speech suitable for robot voice playback, with the most important items prioritized over full event listing.
- Record calendar query and summary generation actions in `StructuredResponse.actions`.
- Preserve `workAssistant.handleEvent` as the only Gateway method.

**Non-Goals:**

- Do not implement meeting notification or Lark IM message sending.
- Do not implement route, weather, travel, or user profile adapters.
- Do not use model-backed summarization inside the plugin.
- Do not add persistent context storage or persistent idempotency.
- Do not change existing calendar creation behavior.

## Decisions

### Route event types to domain handlers

`createWorkAssistantHandler` will dispatch by `InputEvent.type`:

- `user_utterance` continues to route to `CalendarAssistant`.
- `head_touch` and `daily_briefing_triggered` route to `AgendaBriefingAssistant`.
- Unsupported event types keep returning the existing unsupported structured response.

Alternative considered: infer agenda briefing from `payload.text` inside `user_utterance`. Rejected for this change because the target scenario is a robot/system trigger, and keeping the trigger explicit makes the behavior predictable.

### Reuse the calendar adapter with a read method

Extend `LarkCalendarAdapter` with a read-side method such as:

```ts
listEvents(request: {
  start: string;
  end: string;
  calendarId?: string;
}): Promise<LarkCalendarListResult>;
```

The `lark-cli` implementation will use fixed argv arrays around:

```text
lark-cli calendar +agenda --start <iso> --end <iso> --calendar-id <id> --as <identity> --format json
```

Dry-run mode will return deterministic sample agenda data for tests and Gateway smoke checks.

Alternative considered: create a separate `LarkAgendaAdapter`. Rejected because agenda briefing and later meeting reminder/travel planning all need calendar event reads, so the calendar adapter is the right shared boundary.

### Compute briefing windows in the plugin

The agenda assistant will derive windows from `event.timestamp` and `context.timezone`:

- Today's agenda: start and end of the local date containing the event timestamp.
- Recap window: previous Monday through Friday before the target date.

Callers may provide optional payload overrides later, but this change should not require them.

Alternative considered: require OpenClaw to pass explicit windows. Rejected for the MVP because the standard morning briefing has a stable default and fewer caller parameters reduce integration friction.

### Use deterministic classification rules

Calendar classification will be deterministic and configurable through plugin config defaults, for example keyword lists matched against title, location, and description:

- `internal_meeting`
- `customer_reception`
- `outdoor_activity`
- `deep_work`
- `uncategorized`

The classifier should return both a category and a reason/source field for debugging. Rules must live behind a small interface so later implementations can replace keyword matching without changing the assistant contract.

Alternative considered: call a model to classify events. Rejected because the first implementation needs deterministic, testable behavior and no extra dependency.

### Generate bounded speech from structured briefing data

The assistant will build an internal briefing summary containing:

- recap counts by category
- today's total event count
- selected highlights for today's important events
- optional warnings when calendar query data is unavailable

Speech generation will be template-based and bounded. It should name at most a small number of highlights, prefer events with near-term start time, location, external/customer/outdoor indicators, or preparation keywords, and avoid reciting every event in a busy calendar.

Alternative considered: return only structured summary data and let the robot generate speech. Rejected because the existing `StructuredResponse` contract already includes robot voice text, and keeping speech generation in the business layer matches the current plugin boundary.

### Treat calendar reads as actions, not idempotent side effects

Calendar list attempts and summary generation will be recorded in `actions`, but read-only actions should not be cached by the idempotency store. Calendar creation remains the only side-effecting action cached by the current in-memory idempotency behavior.

Alternative considered: cache agenda responses per `event_id`. Rejected because repeated read requests should be allowed to reflect current calendar state unless a future caller explicitly needs snapshot semantics.

## Risks / Trade-offs

- Lark calendar query output may vary across CLI versions -> Normalize only the fields needed by briefing and add parser tests for supported shapes.
- Keyword classification can be inaccurate -> Keep categories configurable and include `uncategorized` rather than forcing a wrong label.
- Busy calendars can produce verbose speech -> Enforce a maximum number of spoken highlights and keep detailed counts in `context_patch` or action details.
- Calendar read permissions may be missing in real environments -> Return a degraded structured response with a failed `lark.calendar.list` action and no external writes.
- Timezone/window mistakes can make briefings feel wrong -> Centralize window calculation and test timestamp/timezone cases.

## Migration Plan

1. Extend TypeScript contracts and handler options for agenda briefing without changing the external `InputEvent` and `StructuredResponse` shapes.
2. Add calendar list types and adapter implementations for dry-run and `lark-cli calendar +agenda`.
3. Implement agenda window calculation, event normalization, classification, summary generation, and speech formatting.
4. Update tests, README, sample fixtures, and packaged skill guidance.
5. Verify with `npm run verify`, OpenSpec validation, and dry-run Gateway invocation for `head_touch` and `daily_briefing_triggered`.

Rollback is to remove the agenda handler registration. Existing `user_utterance` calendar creation behavior remains isolated and should continue to work.

## Open Questions

- Should the first real deployment use `head_touch`, `daily_briefing_triggered`, or both as enabled triggers?
- Should classification keywords be configured in plugin config immediately, or start with built-in defaults and document the future config shape?
- Should recap use previous Monday-Friday only, or the last five business days when the briefing happens mid-week?
