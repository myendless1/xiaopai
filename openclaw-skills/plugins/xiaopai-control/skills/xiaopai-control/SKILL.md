---
name: xiaopai-control
description: Render OpenClaw speech and presentation hints on a local Xiaopai / stack-chan device through the xiaopai-control Gateway plugin.
license: MIT
---

Use this skill when an OpenClaw response should be rendered by the local Xiaopai robot: speech, face expression, face animation, head motion, command sequence, health check, device list, or stop.

## Gateway Methods

- `xiaopaiControl.execute`: queue one validated Xiaopai command.
- `tool.xiaopaiControl.execute`: equivalent execute alias for tool-routed calls.
- `xiaopaiControl.getHealth`: check whether the stack-chan server is reachable.
- `xiaopaiControl.listDevices`: list recently online Xiaopai devices and queue metadata.

## Commands

Use `xiaopaiControl.execute` with either a command object directly or `{ "command": <command> }`.

When using the OpenClaw CLI, call Gateway methods exactly with `openclaw gateway call <method> --params '<json>'`.
Do not use `plugin xiaopai-control ...`, `plugin.xiaopai-control...`, `gateway xiaopaiControl.execute`, or `--data`.

CLI example:

```bash
openclaw gateway call xiaopaiControl.execute --json --params '{"command":{"type":"speak","text":"你好，我是小派同学。","interrupt":true}}'
```

Speak:

```json
{ "type": "speak", "text": "你好，我是小派同学。", "interrupt": true }
```

Face expression:

```json
{ "type": "face", "expression": "thinking" }
```

Face animation:

```json
{ "type": "action", "action": "blink" }
```

Head motion:

```json
{ "type": "move", "direction": "left", "degree": 15, "duration_ms": 500 }
```

Center motion does not need `degree`:

```json
{ "type": "move", "direction": "center" }
```

Sequence:

```json
{
  "type": "sequence",
  "interrupt": true,
  "steps": [
    { "type": "face", "expression": "thinking" },
    { "type": "speak", "text": "我想一下。" },
    { "type": "face", "expression": "happy_squint" }
  ]
}
```

Stop:

```json
{ "type": "stop" }
```

Add `device_id` only when targeting a specific Xiaopai. If omitted, the plugin may apply its configured `defaultDeviceId`; otherwise the stack-chan server chooses the first online device.

## Supported Values

Expressions: `calm`, `shy`, `thinking`, `speak1`, `speak2`, `blink_half`, `blink_closed`, `wink_half`, `wink_closed`, `heart_small`, `heart`, `nod_soft`, `nod_down`, `happy_squint`, `happy_squint_soft`.

Actions: `blink`, `wink`, `heart_action`, `hearting`, `nod`, `nodding`, `speak`, `speaking`, `happy_dynamic`, `happy_squint_dynamic`.

Move directions: `left`, `right`, `up`, `down`, `center`.

## Rendering StructuredResponse

When a stack-chan user message contains `schema: "openclaw.stackchan.event.v1"`, treat the envelope as structured robot input and let the OpenClaw agent choose the handler. Call `workAssistant.handleEvent` only for events that match work-assistant's business capabilities. For ordinary chat, simple Q&A, direct robot expression requests, bare device events such as `head_touch`, or presentation-only commands, the agent can produce the response itself and call `xiaopaiControl.execute` directly. Do not add or require a new Gateway method, and do not make `work-assistant` directly control Xiaopai hardware.

If the envelope event is `work_assistant_proactive_response` with `payload.schema: "openclaw.work_assistant.scheduler_response.v1"`, the work-assistant scheduler has already produced the canonical `StructuredResponse`. Do not call `workAssistant.handleEvent` again; render `payload.structured_response.speech` and supported presentation hints through `xiaopaiControl.execute`.

For envelopes with `render.target: "xiaopai"`, the Xiaopai control plugin has a runtime fallback guard. You should still call `xiaopaiControl.execute` explicitly for planned robot output; the fallback is only a reliability layer. If the final assistant response contains user-facing text and the turn did not successfully queue a speech-capable `xiaopaiControl.execute` command, the runtime normalizes Markdown tables/formatting into speech text, then queues a validated fallback `sequence` with a `speak` step and a final `face: calm` step.

Only successful `speak` commands, or `sequence` commands containing a non-empty `speak` step, count as rendered speech. Face, action, move, stop, health/list calls, rejected results, and failed results do not suppress fallback.

When you already have a `StructuredResponse` from work-assistant, or an equivalent agent-generated robot response with speech/presentation hints, prefer one validated `sequence` command:

- Map `speech` to a `speak` step when non-empty.
- Map `presentation.emotion` to a `face` step only when it is supported or safely mapped.
- Map `presentation.motion` to an `action` step when it is supported or safely mapped.
- Map `presentation.motion` to a `move` step when it is `left`, `right`, `up`, `down`, or `center`.
- Include `device_id` from the stack-chan envelope when available.
- Include `interrupt` from `envelope.render.interrupt` when available.

Conservative presentation mapping:

| Work-assistant hint | Xiaopai command |
| --- | --- |
| `happy`, `positive` | `face: happy_squint` |
| `focused`, `thinking`, `concerned` | `face: thinking` |
| `calm`, `neutral`, `quiet` | `face: calm` |
| `nod`, `small_nod`, `look_at_user` | `action: nod` |
| `blink`, `wink` | matching `action` |
| `left`, `right`, `up`, `down`, `center` | matching `move` |
| `idle`, `none`, `stretch_prompt`, unknown values | omit the step |

Example:

```json
{
  "command": {
    "type": "sequence",
    "device_id": "44:1b:f6:e4:83:8c",
    "interrupt": true,
    "steps": [
      { "type": "face", "expression": "thinking" },
      { "type": "action", "action": "nod" },
      { "type": "speak", "text": "十分钟后有项目同步会。" },
      { "type": "face", "expression": "calm" }
    ]
  }
}
```

If a presentation hint is unsupported, omit that step or use only the conservative mapping above; do not send unsupported expression, action, or motion values to `xiaopaiControl.execute`.

## Fallback Diagnostics

The runtime logs fallback decisions with outcomes `explicit_rendered`, `fallback_rendered`, `fallback_skipped`, or `fallback_failed`. Diagnostics may include stack-chan `event_id`, `device_id`, `event_type`, skip reasons such as `not_stackchan`, `not_xiaopai_target`, `no_final_text`, and `already_rendered`, and sanitized plugin rejection or adapter failure details.

Common failure modes:

- Plugin unavailable or hooks disabled: no fallback hook runs.
- Permission denied for explicit calls: request or configure `operator.write`.
- Validation rejected: check command shape and the 500-character speech limit.
- Adapter/server failure: check stack-chan health and device availability.
- Dry-run mode: queued-like results are returned without contacting stack-chan.

## MCP

MCP is out of scope for this MVP. If external MCP clients need Xiaopai control later, add MCP as an adapter over the same command contract rather than creating a separate device-control schema.
