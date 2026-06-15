## 1. Plugin Scaffold

- [x] 1.1 Create a local `work-assistant` OpenClaw plugin package under this repository.
- [x] 1.2 Add plugin package metadata, `openclaw.plugin.json`, TypeScript config, build scripts, and test scripts.
- [x] 1.3 Register the plugin with `definePluginEntry` and expose a canonical `workAssistant.handleEvent` Gateway method.
- [x] 1.4 Add a minimal runtime smoke test that verifies the plugin registers without starting Lark side effects.

## 2. Event and Response Contracts

- [x] 2.1 Define TypeScript types and runtime validation for `InputEvent`, `StructuredResponse`, `ToolAction`, `PresentationHints`, `FollowUp`, and `ContextPatch`.
- [x] 2.2 Implement the shared assistant event handler that validates event shape and dispatches supported event types.
- [x] 2.3 Return structured unsupported-event responses for non-MVP event types without side effects.
- [x] 2.4 Add idempotency storage keyed by `InputEvent.event_id` for side-effecting operations.
- [x] 2.5 Test successful response shape, follow-up response shape, unsupported event handling, and duplicate-event behavior.

## 3. Calendar Assistant Domain

- [x] 3.1 Implement a `CalendarIntentParser` for the supported Chinese calendar creation utterance patterns.
- [x] 3.2 Resolve relative dates and times from `InputEvent.timestamp` and `context.timezone`.
- [x] 3.3 Implement calendar intent validation for title, date, start time, end time, requester identity, and attendees.
- [x] 3.4 Implement follow-up responses for missing time, invalid time ranges, unsupported utterances, and unresolved ambiguity.
- [x] 3.5 Build successful calendar responses with speech, presentation hints, action records, and context patches.
- [x] 3.6 Test parser, relative time resolution, validation failures, follow-up output, success output, and context patch output.

## 4. Lark Adapter Layer

- [x] 4.1 Define `LarkContactAdapter` and `LarkCalendarAdapter` interfaces independent of `lark-cli`.
- [x] 4.2 Implement process-backed `lark-cli` contact resolution using fixed argv arrays, JSON output, timeouts, and strict parsing.
- [x] 4.3 Implement process-backed `lark-cli` calendar event creation and attendee invitation using fixed argv arrays, JSON output, timeouts, and strict parsing.
- [x] 4.4 Convert Lark adapter success and failure results into stable assistant action records.
- [x] 4.5 Add mocked process tests for unique attendee matches, ambiguous attendees, missing attendees, calendar creation success, and adapter failure.

## 5. Integration and Local Verification

- [x] 5.1 Wire `user_utterance` events through the shared handler into `calendar_assistant`.
- [x] 5.2 Add a safe dry-run or mocked-adapter fixture for invoking `workAssistant.handleEvent` with the sample calendar creation utterance.
- [x] 5.3 Install or link the local plugin into the running OpenClaw environment for verification.
- [x] 5.4 Verify `openclaw plugins inspect work-assistant --runtime --json` reports the plugin runtime correctly.
- [x] 5.5 Verify the Gateway method returns a valid `StructuredResponse` for the sample event.
- [x] 5.6 Verify duplicate submission of the same `event_id` does not create duplicate calendar side effects.

## 6. Companion Skill and Documentation

- [x] 6.1 Add an optional companion OpenClaw skill packaged with the plugin that explains when to call the work assistant capability.
- [x] 6.2 Document that the plugin is the execution layer, the companion skill is guidance only, and MCP is out of scope for this MVP.
- [x] 6.3 Document required Lark scopes, identity assumptions, and how to switch the Lark adapter from `lark-cli` to direct OpenAPI later.
- [x] 6.4 Run OpenSpec validation and plugin tests, then record verification commands and results.
