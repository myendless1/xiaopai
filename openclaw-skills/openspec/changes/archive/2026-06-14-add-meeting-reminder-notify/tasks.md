## 1. Contracts and Routing

- [x] 1.1 Add meeting reminder and notification payload/context types, including `meeting.notify_late` structured intent and `current_focus` meeting shape.
- [x] 1.2 Extend input and structured intent validation so `meeting.notify_late` version `1` is accepted and invalid versions return structured follow-up responses.
- [x] 1.3 Wire `meeting_starting_soon` events to a new meeting reminder domain handler in `createWorkAssistantHandler`.
- [x] 1.4 Route user utterances with `meeting.notify_late` intent, and narrow current-focus late-notification fallback utterances, before falling through to `CalendarAssistant`.

## 2. Meeting Reminder Domain

- [x] 2.1 Create `MeetingReminderAssistant` with proactive reminder handling for valid scheduler `meeting_starting_soon` events.
- [x] 2.2 Generate concise reminder speech, presentation hints, and a successful `meeting.reminder.generate` action from `payload.calendar_event`.
- [x] 2.3 Return a failed `meeting.reminder.generate` action with no external side effect for malformed scheduler reminder events.
- [x] 2.4 Add `context_patch.current_focus` with meeting summary and optional notification target after successful reminders.
- [x] 2.5 Implement bounded late-arrival notification message formatting from structured intent or deterministic fallback parsing.

## 3. Lark IM Adapter

- [x] 3.1 Add `LarkIMAdapter` request/result contracts for sending text to a chat id or attendee user ids.
- [x] 3.2 Implement `DryRunIMAdapter` with deterministic message ids and no `lark-cli` calls.
- [x] 3.3 Implement `LarkCliIMAdapter` following the existing fixed-argv, JSON-output, timeout-aware adapter pattern.
- [x] 3.4 Wire the IM adapter into `createDefaultWorkAssistantRuntime` using the same dry-run, identity, CLI path, and timeout config as other Lark adapters.

## 4. Notification Safety and Idempotency

- [x] 4.1 Send late-arrival notifications only when `context.current_focus` identifies a calendar event and has a usable chat id or attendee ids.
- [x] 4.2 Return a follow-up asking for the meeting or recipients when focus or notification target data is missing.
- [x] 4.3 Record successful and failed IM sends as `lark.message.send` actions with safe details and errors.
- [x] 4.4 Extend side-effect idempotency so successful `lark.message.send` responses are cached by `InputEvent.event_id`.
- [x] 4.5 Preserve focused meeting context for retry after IM adapter failure.

## 5. Scheduler and Fixtures

- [x] 5.1 Extend dry-run calendar or scheduler fixture data with notification target metadata when available.
- [x] 5.2 Add fixture files for proactive `meeting_starting_soon`, structured late notification, missing-target follow-up, and adapter-failure cases.
- [x] 5.3 Update scheduler configuration examples to show explicitly enabling `meetingStartingSoon` with the meeting reminder handler.
- [x] 5.4 Keep `meetingStartingSoon` opt-in by default unless explicitly configured.

## 6. Tests

- [x] 6.1 Add contract tests for `meeting_starting_soon` routing and `meeting.notify_late` structured intent validation.
- [x] 6.2 Add unit tests for reminder speech, action shape, malformed reminder handling, and `current_focus` context patches.
- [x] 6.3 Add unit tests for structured late notification, deterministic fallback parsing, missing focus, and missing notification target follow-ups.
- [x] 6.4 Add adapter tests for dry-run IM sends and CLI IM success, failure, and parse-error handling.
- [x] 6.5 Add idempotency tests proving duplicate notification event ids do not send duplicate Lark messages.
- [x] 6.6 Add scheduler-enabled dry-run smoke tests for `meeting_starting_soon` dispatch through the meeting reminder handler.

## 7. Documentation and Verification

- [x] 7.1 Update `plugins/work-assistant/README.md` with meeting reminder behavior, follow-up notification examples, IM permissions, and scheduler opt-in config.
- [x] 7.2 Update `plugins/work-assistant/skills/work-assistant/SKILL.md` with guidance for scheduler-produced meeting reminders and structured late-notification intents.
- [x] 7.3 Update `plugins/work-assistant/VERIFICATION.md` with test commands and dry-run fixture results.
- [x] 7.4 Run `npm run verify` in `plugins/work-assistant`.
- [x] 7.5 Run `openspec validate add-meeting-reminder-notify --strict`.
