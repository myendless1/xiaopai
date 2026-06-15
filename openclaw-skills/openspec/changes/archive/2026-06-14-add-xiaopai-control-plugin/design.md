## Context

The stack-chan server at `/home/ubuntu/stack-chan/stack-chan-server` already exposes local HTTP endpoints for Xiaopai device health, online devices, queued speech, face expressions, face animations, head motion, command sequences, and stop. The API document recommends `POST /command` for OpenClaw callers and warns that the local endpoints have no authentication.

OpenClaw already has a `work-assistant` plugin that returns structured speech and presentation hints while deliberately avoiding direct hardware control. That boundary should remain intact: business assistants decide what to say and how the robot should present it; a separate device-control adapter renders those results to Xiaopai when the deployment wants physical output.

## Goals / Non-Goals

**Goals:**

- Add a standalone `xiaopai-control` OpenClaw plugin that wraps the stack-chan HTTP API behind Gateway methods.
- Provide one validated command execution method for speech, face expression, face animation, head motion, sequence, and stop operations.
- Provide read-only health and device-list methods for deployment checks and device selection.
- Normalize validation errors, adapter failures, dry-run results, and successful queue responses into stable OpenClaw-facing result objects.
- Include companion skill guidance so agents can render a `StructuredResponse` by mapping `speech` to Xiaopai speech and `presentation.emotion` / `presentation.motion` to supported Xiaopai commands.
- Keep the plugin useful in test and demo environments through dry-run behavior.

**Non-Goals:**

- Do not implement TTS, device polling, servo control, face rendering, or the Xiaopai firmware protocol inside OpenClaw.
- Do not change the `work-assistant` plugin contract or make it directly depend on Xiaopai.
- Do not add an MCP server in the MVP.
- Do not expose unauthenticated stack-chan endpoints beyond the existing trusted LAN deployment assumption.
- Do not add model-backed planning inside the device-control plugin; callers decide the command or sequence to execute.

## Decisions

### Use a standalone OpenClaw plugin

`xiaopai-control` will be a normal plugin package under `plugins/xiaopai-control` using `definePluginEntry`. Its manifest will register:

- `xiaopaiControl.execute`
- `xiaopaiControl.getHealth`
- `xiaopaiControl.listDevices`

The execute method should require operator write scope because it queues audible or visible device side effects. Health and device-list methods can use a read scope if the Gateway supports separate scopes; otherwise they should be documented as read-only even if registered with the same available plugin method mechanism.

Alternative considered: add Xiaopai calls to `work-assistant`. Rejected because `work-assistant` is the business orchestration layer and should keep returning transport-independent structured responses.

Alternative considered: have agents call `curl` directly. Rejected because direct calls bypass validation, timeout handling, dry-run behavior, and stable result shapes.

### Prefer one command contract over many tiny methods

`xiaopaiControl.execute` should accept a discriminated command object:

- `speak`: `{ type: "speak", text, device_id?, interrupt? }`
- `face`: `{ type: "face", expression, device_id?, interrupt? }`
- `action`: `{ type: "action", action, device_id?, interrupt? }`
- `move`: `{ type: "move", direction, degree?, duration_ms?, device_id?, interrupt? }`
- `sequence`: `{ type: "sequence", steps, device_id?, interrupt? }`
- `stop`: `{ type: "stop", device_id? }`

The plugin will translate valid commands to the stack-chan server's `POST /command` JSON entry where possible. Stop can call the existing `GET /command/stop` endpoint unless the upstream server grows a JSON stop command later.

Alternative considered: expose separate Gateway methods for every command type. Rejected for the MVP because a unified method keeps agent guidance simple and makes sequence execution the default composition path.

### Validate before adapter calls

The plugin should define TypeScript types and runtime validators for command input, using fixed allowlists from the stack-chan API document:

- Expressions: `calm`, `shy`, `thinking`, `speak1`, `speak2`, `blink_half`, `blink_closed`, `wink_half`, `wink_closed`, `heart_small`, `heart`, `nod_soft`, `nod_down`, `happy_squint`, `happy_squint_soft`
- Actions: `blink`, `wink`, `heart_action`, `hearting`, `nod`, `nodding`, `speak`, `speaking`, `happy_dynamic`, `happy_squint_dynamic`
- Move directions: `left`, `right`, `up`, `down`, `center`

Validation should reject empty speech text, unknown command names, invalid sequence steps, and unsafe numeric bounds before calling the HTTP adapter. `center` should not require a degree value.

### Keep result shape stable and action-oriented

The plugin should return a `XiaopaiCommandResult` rather than leaking raw stack-chan responses:

```json
{
  "status": "queued",
  "device_id": "robot-001",
  "action": {
    "type": "xiaopai.command",
    "status": "success",
    "details": {
      "command_type": "sequence",
      "cmd_id": "cmd_xxx"
    }
  }
}
```

Validation errors should return `status: "rejected"` with an action status of `failed` or `skipped` and a stable error code. Adapter failures should return `status: "failed"` with the HTTP or timeout details sanitized for troubleshooting.

### Use adapters for real HTTP and dry-run

Implementation should keep stack-chan I/O behind an adapter interface. The real adapter uses `fetch`, configured `baseUrl`, and `timeoutMs`. The dry-run adapter returns deterministic queued-like responses and never performs network I/O. Tests should exercise validation and handler behavior primarily through dry-run and mocked adapters.

### Companion skill is guidance, not execution

The plugin should package a skill that tells OpenClaw agents to call `xiaopaiControl.execute` for robot presentation. The skill should include mappings such as:

- `StructuredResponse.speech` -> `speak`
- `presentation.emotion` values that match supported Xiaopai expressions -> `face`
- `presentation.motion` values such as `nod`, `left`, `right`, `center` -> action or move steps

The skill should instruct agents to prefer a `sequence` when rendering multiple presentation actions around speech.

## Risks / Trade-offs

- Stack-chan server is offline or no device is online -> health/list methods expose deployment state; command adapter failures return structured failed actions.
- Stack-chan API shape changes -> keep the plugin adapter small and covered by unit tests; normalize responses so callers are insulated from minor upstream changes.
- Hardware side effects are duplicated by repeated calls -> MVP should expose `interrupt` and rely on callers to provide stable interaction flow; a later change can add command idempotency if repeated physical playback becomes a problem.
- Unsupported presentation hint values from business plugins -> companion skill and validators constrain execution to supported expressions/actions and reject unknown values without calling hardware.
- Local endpoints have no auth -> document trusted-LAN assumption and keep OpenClaw plugin as the normal call path rather than encouraging direct network exposure.

## Migration Plan

1. Add and build the new plugin package without changing existing `work-assistant` behavior.
2. Install or link the plugin into the local OpenClaw environment.
3. Configure `dryRun: true` for tests and initial Gateway inspection.
4. Configure `baseUrl` for the stack-chan server and verify `getHealth` and `listDevices`.
5. Run a small real-device smoke test for `speak`, `face`, `move center`, and a short `sequence`.
6. Rollback by disabling or uninstalling the `xiaopai-control` plugin; existing work-assistant behavior remains unaffected.

## Open Questions

- What Gateway scope names should be used for read-only plugin methods if the local OpenClaw version distinguishes read and write operator permissions?
- Should a later change add command idempotency keys for physical playback, or is caller-level event idempotency enough for the first hardware-rendering MVP?
- Should the companion skill include a stricter canonical mapping from all existing `work-assistant` presentation hints to Xiaopai expressions and motions?
