## Context

`work-assistant` already has the pieces around the meeting reminder flow, but they are not connected into a user-visible capability. The proactive calendar scheduler can create `meeting_starting_soon` plans from Lark calendar data and dispatch normalized `InputEvent` objects. The shared event contract already defines scheduler-produced metadata, and `workAssistant.handleEvent` is the stable transport-independent boundary.

The gap is in domain handling: `handler.ts` currently routes agenda and wellbeing events, then sends all remaining supported user utterances to `CalendarAssistant`. A `meeting_starting_soon` event is still unsupported at the handler layer, and a follow-up utterance like "I will be five minutes late, notify the attendees" has no meeting context resolution or Lark IM side-effect path.

This change should make the smallest useful proactive loop work:

```text
scheduler meeting_starting_soon
        │
        ▼
MeetingReminderAssistant
        │
        ├─ reminder speech + presentation hints
        ├─ meeting.reminder.generate action
        └─ context_patch.current_focus
                  │
                  ▼
follow-up user_utterance
        │
        ▼
meeting.notify_late intent / constrained fallback
        │
        ▼
LarkIMAdapter.sendMessage
```

## Goals / Non-Goals

**Goals:**

- Route `meeting_starting_soon` events to a dedicated meeting reminder domain assistant.
- Generate concise meeting reminder responses from scheduler payloads without re-reading the calendar in the common path.
- Store the reminded meeting in `context_patch.current_focus` so immediate user follow-ups can refer to the meeting implicitly.
- Support a narrow late-arrival notification follow-up that sends a Lark IM message when a safe recipient target is available.
- Add a `LarkIMAdapter` abstraction with dry-run and CLI-backed implementations.
- Protect Lark IM sends with `InputEvent.event_id` idempotency and report them as `lark.message.send` actions.
- Provide fixtures, tests, README guidance, packaged skill guidance, and verification notes.

**Non-Goals:**

- Do not implement travel planning, weather lookup, route lookup, business trip advice, or outdoor event reminders.
- Do not implement meeting transcription, meeting minutes, RSVP changes, room booking, or calendar event updates.
- Do not infer arbitrary follow-up intent from broad free-form text without current meeting context.
- Do not send Lark messages when the target chat or attendee recipients are missing or ambiguous.
- Do not add a new public Gateway method; use `workAssistant.handleEvent`.
- Do not make `meetingStartingSoon` enabled for every deployment without explicit scheduler configuration.

## Decisions

### Add a MeetingReminderAssistant behind the existing handler

Create a `MeetingReminderAssistant` module with methods for proactive reminders and meeting notification follow-ups. `createWorkAssistantHandler` should route `meeting_starting_soon` directly to this assistant, then route user utterances with a meeting notification structured intent or safe fallback match before falling through to `CalendarAssistant`.

Alternative considered: add reminder logic to `AgendaBriefingAssistant`. Rejected because agenda briefing is read-only summarization, while meeting notifications introduce current-focus context and Lark IM side effects.

### Use scheduler payloads as the reminder source of truth

The reminder handler should read `payload.trigger` and `payload.calendar_event`. It should compute the visible reminder from the calendar event start time, fired time, title, and location. It should not call a model or perform another calendar scan to decide whether to remind.

If the scheduler event is malformed or lacks `payload.calendar_event`, return a structured response with a failed `meeting.reminder.generate` action and no external side effect.

Alternative considered: fetch full calendar event details before every reminder. Rejected for the first implementation because the scheduler already normalized the relevant fields, and extra reads increase latency and failure modes. A future detail resolver can be added if Lark attendee or chat data is not present in scheduler payloads.

### Preserve focus through context_patch.current_focus

A successful reminder response should include a context patch like:

```json
{
  "current_focus": {
    "type": "calendar_event",
    "event_id": "calendar_event_id",
    "calendar_id": "primary",
    "title": "项目会",
    "start_time": "2026-06-06T11:00:00+08:00",
    "end_time": "2026-06-06T12:00:00+08:00",
    "location": "中1会议室",
    "notification_target": {
      "chat_id": "oc_xxx"
    }
  }
}
```

`notification_target` should be optional. The assistant can send a follow-up notification only if the current focus contains a usable chat id or attendee ids. If the target is absent, the assistant asks a concise follow-up question instead of guessing recipients.

Alternative considered: store meeting focus only in an internal plugin memory store. Rejected because the surrounding OpenClaw/robot flow already passes short-lived context through `InputEvent.context`, and `context_patch` is the existing mechanism for caller-managed state.

### Prefer structured notification intent with a constrained text fallback

The preferred follow-up path is a structured intent in `payload.structured_intent`:

```json
{
  "type": "meeting.notify_late",
  "version": "1",
  "delay_minutes": 5,
  "message": "我会晚五分钟到，请大家稍等一下"
}
```

The plugin may also support a narrow deterministic fallback when `context.current_focus.type` is `calendar_event` and the utterance clearly combines late-arrival wording with notification wording. The fallback should extract only simple minute delays and use bounded message templates.

Alternative considered: let the plugin perform broad natural-language understanding. Rejected to keep the plugin deterministic and consistent with the existing OpenClaw extraction boundary.

### Add LarkIMAdapter and record Lark message side effects

Add a `LarkIMAdapter` interface that can send text to a chat id or a list of user ids. The dry-run implementation should return deterministic message ids. The CLI-backed implementation should follow the existing fixed-argv, JSON-output pattern used by the contact and calendar adapters.

The handler idempotency rule should treat successful `lark.message.send` actions as side effects, the same way it currently protects calendar creation. Duplicate `InputEvent.event_id` submissions for a notification follow-up must not send duplicate messages.

Alternative considered: call `lark-cli` directly from `MeetingReminderAssistant`. Rejected because direct process calls inside domain logic would make tests and future OpenAPI adapter support harder.

### Keep scheduler activation explicit

This change should not require new scheduler scan behavior. The existing `meetingStartingSoon` rule can produce events when explicitly enabled. Documentation and tests should show enabling that rule with the meeting handler present, but production deployments should still opt in through scheduler configuration.

Alternative considered: enable `meetingStartingSoon` by default whenever the scheduler is enabled. Rejected because proactive speech can be noisy and deployments may need a staged rollout.

## Risks / Trade-offs

- Lark calendar agenda data may not include attendee ids or a meeting chat id -> Include optional notification target fields when available, and ask the user to specify a target when not available.
- Free-form follow-up parsing can steal unrelated calendar creation utterances -> Prefer structured intent and only run fallback when current meeting focus exists and the wording clearly requests notification.
- Duplicate follow-up submissions could send duplicate IM messages -> Extend idempotency detection to cache successful `lark.message.send` responses by `event_id`.
- Lark IM permissions may differ for user and bot identities -> Surface adapter failures as failed `lark.message.send` actions and document required permissions.
- Scheduler can dispatch before the handler is configured to handle notifications -> Meeting reminder responses do not require IM permissions, and the scheduler rule remains explicit opt-in.
- Current focus can become stale if the user waits too long -> Use the event start/end times from context and ask for clarification if the focused meeting is no longer relevant.

## Migration Plan

1. Add meeting reminder contracts, notification intent validation helpers, and optional notification target fields.
2. Add `MeetingReminderAssistant` for proactive reminder and follow-up notification handling.
3. Add `LarkIMAdapter`, dry-run implementation, and CLI-backed implementation.
4. Wire the assistant and adapter into the plugin factory and handler routing.
5. Extend idempotency to include successful `lark.message.send` actions.
6. Add fixtures and tests for reminder dispatch, current-focus context, structured notification intent, fallback parsing, missing target follow-up, IM adapter failures, and duplicate notification idempotency.
7. Update README, packaged skill guidance, scheduler configuration examples, and verification notes.
8. Validate with `npm run verify`, `openspec validate add-meeting-reminder-notify --strict`, and dry-run Gateway fixtures.

Rollback is to disable the `meetingStartingSoon` scheduler rule and remove the meeting handler routing. Existing agenda briefing, calendar creation, wellbeing, and scheduler daily briefing behavior should continue to work.

## Open Questions

- Which Lark source reliably provides meeting chat ids for calendar events in the target deployment?
- Should notification follow-ups send to a meeting chat when available, or directly to attendee user ids when both are present?
- How long should `current_focus` remain valid for late-arrival follow-ups: until meeting end, a fixed timeout, or caller-managed context lifetime?
