## Why

The work assistant can already create Lark calendar events, but it cannot yet handle the morning briefing scenario that motivated the broader capability design. Adding agenda briefing is the next low-risk step because it is primarily read-only, reuses the existing event/response boundary, and establishes the calendar query and summarization foundation needed by meeting reminders and travel planning.

## What Changes

- Add an `agenda_briefing` domain capability for daily agenda briefing and recent-work recap.
- Support non-utterance assistant events such as `head_touch` and `daily_briefing_triggered` as triggers for agenda briefing.
- Add Lark calendar query behavior for a requested briefing window, including today's agenda and a previous work-week recap window.
- Add configurable calendar event classification for internal meetings, customer receptions, outdoor activities, deep work, and uncategorized events.
- Generate concise robot-speakable briefing responses that prioritize the most important agenda items and avoid dumping full calendar lists.
- Record read-side actions such as `lark.calendar.list` and summary generation in `StructuredResponse.actions`.
- Keep calendar creation behavior unchanged, and do not add meeting notification, route/weather lookup, or wellbeing companion behavior in this change.

## Capabilities

### New Capabilities

- `agenda-briefing`: Daily agenda briefing and recent calendar recap behavior, including briefing trigger events, calendar querying, event classification, highlight selection, concise speech generation, and action reporting.

### Modified Capabilities

- None.

## Impact

- Affects `plugins/work-assistant/src/handler.ts`, new agenda briefing domain files, Lark calendar adapter interfaces/implementations, dry-run adapters, tests, README, and packaged skill guidance.
- Adds read-only Lark calendar list usage through the existing plugin execution boundary.
- Requires the Lark runtime identity to have calendar read/list permissions in addition to the existing calendar create/contact scopes.
- Reuses `workAssistant.handleEvent` as the Gateway method; no new transport surface is required.
- Provides shared calendar query and classification infrastructure for later meeting reminder, travel planning, and wellbeing follow-up proposals.
