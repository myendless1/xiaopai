## 1. Stack-Chan JSON Event Forwarding

- [x] 1.1 Replace stack-chan's prose OpenClaw event builder with a JSON envelope builder using schema `openclaw.stackchan.event.v1`.
- [x] 1.2 Map non-empty speech recognition results to `InputEvent` objects with `type: "user_utterance"` and recognized text in `payload.text`.
- [x] 1.3 Map non-touch device events to structured `InputEvent` objects while preserving device id, event name, source event type, timestamp, and timezone context.
- [x] 1.4 Ensure the OpenClaw chat-completions request sends the JSON envelope as the user message content and keeps the per-device `x-openclaw-session-key`.

## 2. Stack-Chan Command Boundary

- [x] 2.1 Preserve the local `head_touch`/`touch` face-command shortcut before OpenClaw forwarding.
- [x] 2.2 Ensure OpenClaw-forwarded events return `queued_commands: []` unless a command was explicitly queued outside the delegated event path.
- [x] 2.3 Keep stack-chan from parsing OpenClaw response text or tags for Xiaopai commands.
- [x] 2.4 Update stack-chan HTTP command API documentation to describe structured event forwarding and OpenClaw-owned robot presentation.

## 3. Companion Skill Guidance

- [x] 3.1 Update `plugins/work-assistant/skills/work-assistant/SKILL.md` with instructions for recognizing `openclaw.stackchan.event.v1` envelopes and calling `workAssistant.handleEvent` with the embedded event only for work-assistant-supported business events.
- [x] 3.2 Add guidance for preserving `StructuredResponse.context_patch` across the same stack-chan device session when follow-up context is available.
- [x] 3.3 Update `plugins/xiaopai-control/skills/xiaopai-control/SKILL.md` with the required post-`workAssistant.handleEvent` rendering step through `xiaopaiControl.execute`.
- [x] 3.4 Add conservative presentation hint mappings for common `work-assistant` values such as `happy`, `positive`, `focused`, `concerned`, `look_at_user`, `small_nod`, and unsupported hints.
- [x] 3.5 Ensure both skill updates state that no new Gateway method is required and that `work-assistant` must not directly control Xiaopai hardware.
- [x] 3.6 Clarify that ordinary chat, simple Q&A, direct robot expression requests, and presentation-only commands can be handled directly by the OpenClaw agent and rendered with `xiaopaiControl.execute`.

## 4. Verification

- [x] 4.1 Add or update stack-chan tests for speech and non-touch JSON envelope generation.
- [x] 4.2 Add or update stack-chan tests proving `head_touch` events enqueue the local face shortcut and do not forward to OpenClaw.
- [x] 4.3 Run stack-chan tests covering event forwarding and command queue behavior.
- [x] 4.4 Run `openspec validate route-stackchan-json-events-to-xiaopai --strict`.
- [x] 4.5 Run a local smoke test with stack-chan forwarding a structured event to OpenClaw and confirm Xiaopai behavior is produced through `xiaopaiControl.execute`.
