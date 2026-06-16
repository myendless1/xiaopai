# Xiaopai Control Plugin

OpenClaw plugin for validated Xiaopai / stack-chan device control. It exposes Gateway methods for command execution, health checks, and online device listing while keeping stack-chan HTTP response details behind stable OpenClaw-facing result objects.

## Configuration

Default configuration targets a local stack-chan server:

```json
{
  "baseUrl": "http://127.0.0.1:8091",
  "timeoutMs": 5000,
  "dryRun": false
}
```

Optional single-device configuration:

```json
{
  "baseUrl": "http://127.0.0.1:8091",
  "defaultDeviceId": "AA:BB:CC:DD:EE:FF",
  "timeoutMs": 5000,
  "dryRun": false
}
```

For tests, demos, and Gateway inspection without hardware:

```json
{ "dryRun": true }
```

Dry-run mode performs no network calls. It returns deterministic queued-like command results, an OK health response, and one synthetic online device.

## Gateway Methods

- `xiaopaiControl.execute`: validates and queues `speak`, `face`, `action`, `move`, `sequence`, and `stop` commands.
- `xiaopaiControl.getHealth`: normalizes `GET /health`.
- `xiaopaiControl.listDevices`: normalizes `GET /devices`.

`xiaopaiControl.execute` is registered with `operator.write` because it can create audible and visible hardware side effects. Health and device-list methods are registered with `operator.read`. If a local Gateway version does not distinguish read/write scopes, treat health and list calls as read-only by convention.

## Stack-Chan Render Fallback

The plugin also registers runtime hooks for stack-chan turns whose user message is a JSON envelope with `schema: "openclaw.stackchan.event.v1"` and `render.target: "xiaopai"`.

Agents should still call `xiaopaiControl.execute` explicitly whenever they plan robot speech or presentation. The fallback is only a reliability guard: if the turn reaches final assistant text without a successful speech-capable `xiaopaiControl.execute` call, the plugin queues a validated fallback `sequence` with a `speak` step for that final text and a final `face: calm` step.

Fallback behavior:

- A successful explicit `speak` command, or `sequence` containing a non-empty `speak` step, suppresses fallback.
- Face, action, move, stop, health/list calls, rejected results, and failed results do not suppress fallback.
- Final text is trimmed, diagnostic/tool transport lines are removed, Markdown tables/formatting are normalized for speech, raw JSON-only text is skipped, and speech is capped at 500 characters before validation.
- `device_id` comes from the envelope when present, falling back to `event.payload.device_id`; `interrupt` comes from `render.interrupt` and defaults to `true`.
- The OpenAI-compatible response shape is unchanged. Stack-chan does not need to parse fallback metadata.

Diagnostics are written to plugin logs with outcomes `explicit_rendered`, `fallback_rendered`, `fallback_skipped`, or `fallback_failed`. When available, diagnostics include `event_id`, `device_id`, `event_type`, skip reasons such as `not_stackchan`, `not_xiaopai_target`, `no_final_text`, and `already_rendered`, plus sanitized plugin rejection or adapter failure details.

Common failure modes:

- Plugin unavailable or hooks not enabled: no fallback hook runs; explicit agent calls still work when the Gateway method is available.
- Permission denied: explicit user/tool calls may require `operator.write`; fallback runs inside the loaded plugin runtime and still uses the same validated handler boundary.
- Validation rejected: diagnostics report `fallback_failed` with `plugin_rejected`.
- Adapter/server failure: diagnostics report `fallback_failed` with `plugin_failed` or `exception`.
- Dry-run mode: fallback returns queued-like results with `details.dry_run: true` and does not contact stack-chan.

## Command Examples

Speak:

```json
{
  "command": {
    "type": "speak",
    "text": "你好，我是小派同学。",
    "interrupt": true
  }
}
```

Face expression:

```json
{ "command": { "type": "face", "expression": "happy_squint" } }
```

Move center:

```json
{ "command": { "type": "move", "direction": "center" } }
```

Sequence:

```json
{
  "command": {
    "type": "sequence",
    "interrupt": true,
    "steps": [
      { "type": "face", "expression": "thinking" },
      { "type": "move", "direction": "left", "degree": 15, "duration_ms": 500 },
      { "type": "speak", "text": "我往左看一下。" },
      { "type": "face", "expression": "calm" }
    ]
  }
}
```

Stop:

```json
{ "command": { "type": "stop" } }
```

## Stack-Chan Server Assumptions

The plugin expects an already running stack-chan server exposing:

- `POST /command`
- `GET /command/stop`
- `GET /health`
- `GET /devices`

The default server URL is `http://127.0.0.1:8091`. When `device_id` is omitted and no `defaultDeviceId` is configured, the stack-chan server targets the first currently online Xiaopai device.

Stack-chan local endpoints do not provide authentication. Keep them on a trusted LAN or loopback interface and expose Xiaopai control through OpenClaw Gateway permissions instead of publishing stack-chan directly.

## Development

```bash
npm install
npm run build
npm test
```

See [docs/VERIFICATION.md](docs/VERIFICATION.md) for Gateway and optional real-device checks.
