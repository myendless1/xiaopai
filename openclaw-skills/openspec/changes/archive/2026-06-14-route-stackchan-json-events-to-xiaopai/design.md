## Context

The stack-chan server already has two separate responsibilities: it receives Xiaopai device input and it exposes command endpoints that the device polls through `/device/next-command`. The newer `xiaopai-control` plugin wraps those command endpoints behind validated Gateway methods, and `work-assistant` already returns transport-independent `StructuredResponse` objects.

The remaining mismatch is the OpenClaw ingress shape. Stack-chan currently converts OpenClaw-bound events into prose before calling OpenClaw's OpenAI-compatible `/chat/completions` endpoint. For the intended robot orchestration boundary, stack-chan should report eligible OpenClaw events as structured input; OpenClaw should decide the assistant response; `xiaopai-control` should render speech, expressions, actions, or motion by queueing stack-chan commands. The existing `head_touch`/`touch` local face shortcut remains an explicit immediate-feedback exception and is not part of the OpenClaw forwarding path.

## Goals / Non-Goals

**Goals:**

- Send stack-chan-originated OpenClaw messages as JSON event envelopes containing a work-assistant-compatible `InputEvent`.
- Ensure forwarded stack-chan events do not directly enqueue local Xiaopai commands before OpenClaw decides the response, while preserving the local `head_touch`/`touch` shortcut outside the forwarded path.
- Teach the `work-assistant` companion skill that stack-chan JSON event envelopes are structured input, and that `workAssistant.handleEvent` should be called only for work-assistant-supported business events.
- Teach the `xiaopai-control` companion skill to render robot responses through `xiaopaiControl.execute`, using a validated `sequence` when possible, whether the response came from work-assistant or direct Agent handling.
- Preserve the current deployment path through OpenClaw's OpenAI-compatible `/chat/completions` endpoint.

**Non-Goals:**

- Do not add a new OpenClaw Gateway method, HTTP route, MCP server, or plugin dependency.
- Do not make `work-assistant` directly call `xiaopai-control`.
- Do not add deterministic `StructuredResponse` rendering code to `xiaopai-control` in this change.
- Do not implement new robot perception, TTS, firmware protocol, or command queue behavior.
- Do not broaden stack-chan's unauthenticated command API exposure.

## Decisions

### Use a JSON event envelope as the chat-completions user message

Stack-chan should replace prose event descriptions with compact JSON in the user message content. The envelope should be easy for the OpenClaw agent to identify and should contain the actual `InputEvent` to pass through:

```json
{
  "schema": "openclaw.stackchan.event.v1",
  "source": "stack-chan",
  "device_id": "44:1b:f6:e4:83:8c",
  "event": {
    "event_id": "stackchan-44:1b:f6:e4:83:8c-task-123",
    "type": "user_utterance",
    "timestamp": "2026-06-11T17:30:00+08:00",
    "user_id": "ou_requester",
    "payload": {
      "text": "帮我看一下今天日程",
      "device_id": "44:1b:f6:e4:83:8c",
      "source_event_type": "speech_recognition"
    },
    "context": {
      "timezone": "Asia/Shanghai",
      "device_id": "44:1b:f6:e4:83:8c"
    }
  },
  "render": {
    "target": "xiaopai",
    "interrupt": true
  }
}
```

For speech recognition, stack-chan should map to `InputEvent.type: "user_utterance"` and put recognized text in `payload.text`. For non-touch device events eligible for OpenClaw forwarding, stack-chan should preserve the event type when it is supported by `work-assistant`, and include original device fields such as `name`, `source_event_type`, and `device_id` in `payload`. `head_touch` and `touch` should keep using the local face shortcut and return before OpenClaw forwarding.

Alternative considered: add a direct stack-chan HTTP route that calls `workAssistant.handleEvent`. Rejected for this change because the current deployment already uses chat completions and the user requested only stack-chan event JSON plus companion skill updates.

### Keep stack-chan from queuing presentation side effects for forwarded events

When OpenClaw forwarding is enabled and an event is eligible to send, stack-chan should not also enqueue local speech, face, action, or movement commands for that same event. The response body can still report `openclaw_sent`, `queued_commands`, and skip reasons, but `queued_commands` should be empty for events delegated to OpenClaw.

Wake/sleep session management can remain as local transport behavior when required to avoid accidental always-on listening, but robot presentation for handled user events should come back through `xiaopaiControl.execute`.

`head_touch`/`touch` remains a local shortcut: stack-chan queues `face: shy`, reports `openclaw_sent: false`, and returns. Alternative considered: also forward the touch event to OpenClaw. Rejected because duplicate physical feedback makes it unclear whether OpenClaw or stack-chan owns robot behavior.

### Use existing plugin methods with Agent-owned routing

The companion skills should instruct the OpenClaw agent to treat the stack-chan envelope as a structured transport format, not as a mandatory `work-assistant` dispatch. The agent should inspect `envelope.event.type`, `payload.text`, and any structured intent, then choose one of two paths:

1. For work-assistant-supported business events, call `workAssistant.handleEvent` with `{ "event": <envelope.event> }`, then convert the returned `StructuredResponse` into a Xiaopai command and call `xiaopaiControl.execute` when robot presentation is needed.
2. For ordinary chat, simple Q&A, direct robot expression requests, and presentation-only commands, let the Agent handle the event directly and call `xiaopaiControl.execute` when robot presentation is needed.

In either path, the preferred Xiaopai command is a single `sequence` with:

- an optional `face` step from supported or mapped emotion hints,
- an optional `action` or `move` step from supported or mapped motion hints,
- a `speak` step when `StructuredResponse.speech` is non-empty,
- a final calm face step when useful.

Alternative considered: route every stack-chan envelope through `workAssistant.handleEvent`. Rejected because the envelope is a transport shape; ordinary conversation and presentation-only commands do not need the business plugin. A new `xiaopaiControl.renderStructuredResponse` method was also considered and rejected for this change because the requested scope is limited to stack-chan JSON forwarding and skill guidance.

### Document conservative presentation mapping in skill guidance

The `xiaopai-control` skill should not rely only on exact hint matches because existing `work-assistant` responses include presentation values that are not Xiaopai allowlist values. The guidance should include a conservative mapping such as:

- `happy`, `positive` -> `face: happy_squint`
- `focused`, `thinking`, `concerned` -> `face: thinking`
- `calm`, `neutral`, `quiet` -> `face: calm`
- `nod`, `small_nod`, `look_at_user` -> `action: nod`
- `blink`, `wink` -> matching `action`
- `left`, `right`, `up`, `down`, `center` -> `move`
- `idle`, `none`, `stretch_prompt`, unknown values -> omit the unsupported step

This keeps invalid hints out of `xiaopaiControl.execute` while allowing useful robot feedback without changing plugin code.

## Risks / Trade-offs

- Model does not follow the envelope routing instructions -> Keep the envelope schema name explicit and update both companion skills with the exact two-step procedure and command examples.
- Device-side events lose immediate feedback -> Touch events keep local immediate feedback; forwarded events use OpenClaw-rendered sequences as the single owner of robot behavior.
- Unsupported presentation hints are dropped -> Skill guidance provides a small mapping table and instructs agents to omit unknown values rather than sending invalid commands.
- Follow-up context may not persist across sessions -> Stack-chan keeps using a stable `x-openclaw-session-key` per device; future work can add explicit context persistence if needed.
- Implementation touches stack-chan outside this planning repo -> The OpenSpec artifacts live here, but the implementation step must intentionally edit `/home/ubuntu/stack-chan/stack-chan-server` as the affected service.

## Migration Plan

1. Update stack-chan OpenClaw event building so `_call_openclaw` sends JSON envelope content instead of prose.
2. Preserve the local `head_touch`/`touch` shortcut so touch feedback remains immediate and is not forwarded.
3. Update stack-chan API documentation to show structured OpenClaw forwarding for eligible events and the local touch shortcut boundary.
4. Update `work-assistant` and `xiaopai-control` companion skills with the stack-chan envelope routing and response-rendering procedure.
5. Verify first with a dry or no-device event path, then with `xiaopai-control` connected to the local stack-chan server.

Rollback is to restore prose event forwarding and the previous companion skill text; this does not require plugin config changes.

## Open Questions

- What `user_id` should stack-chan use for robot-originated events when no Lark user has been resolved yet: a configured OpenClaw/Lark user id, a stable robot user placeholder, or an omitted workflow-specific value after future contract changes?
- Should wake-only speech such as "我在" remain a local transport-level exception, or should it also be routed through OpenClaw for a fully strict "no local presentation" boundary?
