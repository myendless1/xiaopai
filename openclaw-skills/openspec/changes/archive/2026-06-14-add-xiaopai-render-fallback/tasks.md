## 1. Runtime Hook And Context Detection

- [x] 1.1 Locate the OpenClaw chat-completions or agent turn orchestration point that has access to incoming messages, Gateway/tool call results, and final assistant text.
- [x] 1.2 Add a stack-chan envelope parser that detects `schema: "openclaw.stackchan.event.v1"` and `render.target: "xiaopai"` without treating invalid JSON as an error for normal chat.
- [x] 1.3 Build a per-turn Xiaopai render context containing `device_id`, `interrupt`, `event_id`, `event.type`, and a required-render flag.
- [x] 1.4 Add a local rollback switch or config guard if the runtime already has feature-flag conventions; otherwise keep the guard enabled only for explicit `render.target: "xiaopai"` envelopes.

## 2. Speech Render Observation

- [x] 2.1 Observe `xiaopaiControl.execute` calls made during the current turn through the existing Gateway/tool execution path.
- [x] 2.2 Normalize command inputs accepted by `xiaopai-control`, including both direct command objects and `{ "command": <command> }` wrappers.
- [x] 2.3 Mark a turn as speech-rendered only when a successful result is returned for a `speak` command or a `sequence` command containing a non-empty `speak` step.
- [x] 2.4 Ensure face, action, move, stop, health/list calls, rejected results, and failed results do not satisfy the spoken render requirement.

## 3. Fallback Execution

- [x] 3.1 Extract the final user-facing assistant text at the end of the turn and skip fallback for empty, tool-only, or diagnostic-only responses.
- [x] 3.2 Normalize fallback speech text by stripping transport/debug artifacts, trimming whitespace, and fitting the `xiaopai-control` speech limit of 500 characters.
- [x] 3.3 Build a fallback `sequence` command with a `speak` step, optional final `face: calm` step, envelope `device_id` when available, and envelope `interrupt` when available.
- [x] 3.4 Call the existing `xiaopaiControl.execute` Gateway method for fallback execution without bypassing plugin validation.
- [x] 3.5 Preserve the normal OpenAI-compatible chat response shape after fallback execution.

## 4. Diagnostics And Failure Handling

- [x] 4.1 Record `explicit_rendered`, `fallback_rendered`, `fallback_skipped`, and `fallback_failed` outcomes in runtime logs or internal response metadata.
- [x] 4.2 Include stack-chan `event_id`, `device_id`, and skip/failure reason in diagnostics when available.
- [x] 4.3 Surface plugin rejection or adapter failure details as sanitized diagnostics without throwing away the original final assistant text.
- [x] 4.4 Ensure stack-chan does not need to parse new response metadata to preserve the reliability behavior.

## 5. Tests

- [x] 5.1 Add unit tests for stack-chan envelope detection, non-Xiaopai skip behavior, invalid JSON tolerance, and render context extraction.
- [x] 5.2 Add unit tests proving successful `speak` and `sequence`-with-`speak` calls suppress fallback.
- [x] 5.3 Add unit tests proving action-only, failed, and rejected `xiaopaiControl.execute` calls do not suppress fallback.
- [x] 5.4 Add unit tests proving final text fallback builds the expected `sequence` with `device_id`, `interrupt`, and normalized speech text.
- [x] 5.5 Add tests for no-final-text skip behavior and over-500-character text normalization.
- [x] 5.6 Add an integration or smoke test with `xiaopai-control` dry-run mode showing that a text-only stack-chan turn queues a fallback Xiaopai speech command.

## 6. Documentation And Verification

- [x] 6.1 Update companion skill or integration documentation to explain that explicit `xiaopaiControl.execute` remains preferred and the runtime fallback is only a reliability guard.
- [x] 6.2 Document diagnostics and common failure modes, including plugin unavailable, permission denied, validation rejected, and dry-run behavior.
- [x] 6.3 Run the relevant runtime and plugin tests for the implementation area.
- [x] 6.4 Run `openspec validate add-xiaopai-render-fallback --strict`.
- [x] 6.5 If stack-chan and Xiaopai are available, run a real-device smoke test where OpenClaw intentionally returns text without an explicit plugin call and confirm Xiaopai speaks via fallback.
