# Xiaopai Control Verification

Run these commands from `plugins/xiaopai-control`.

## Dry-Run Build And Tests

```bash
npm install
npm run build
npm test
```

Dry-run behavior can be inspected through the plugin runtime with:

```json
{ "dryRun": true }
```

Expected behavior:

- `xiaopaiControl.execute` returns `status: "queued"` with `details.dry_run: true`.
- `xiaopaiControl.getHealth` returns `status: "ok"` and `service: "xiaopai-dry-run"`.
- `xiaopaiControl.listDevices` returns one `dry-run-device`.
- A text-only stack-chan turn with `render.target: "xiaopai"` queues a fallback `sequence` in unit tests without contacting stack-chan.
- No stack-chan network request is made.

## Gateway Install / Inspect

Use the local OpenClaw command available in the environment. Typical checks:

```bash
openclaw plugins install ./plugins/xiaopai-control
openclaw plugins inspect xiaopai-control --runtime --json
```

The runtime inspect output should include:

- `xiaopaiControl.execute`
- `xiaopaiControl.getHealth`
- `xiaopaiControl.listDevices`

The plugin runtime should also register fallback hooks for `before_agent_run`, `after_tool_call`, `before_agent_finalize`, and `agent_end` on Gateway versions that expose those hook surfaces.

The execute method should be treated as `operator.write`. Health and device-list methods should be treated as read-only; this plugin registers them with `operator.read` when the local Gateway supports method scopes.

## Optional Real-Device Smoke Tests

Only run these when the stack-chan server is running and a Xiaopai device is online.

Check stack-chan directly:

```bash
curl --noproxy 127.0.0.1,localhost http://127.0.0.1:8091/health
curl --noproxy 127.0.0.1,localhost http://127.0.0.1:8091/devices
```

Gateway calls should cover:

- Health check.
- Device list.
- Speech: `{ "type": "speak", "text": "你好，小派。" }`
- Face: `{ "type": "face", "expression": "thinking" }`
- Move center: `{ "type": "move", "direction": "center" }`
- Stop: `{ "type": "stop" }`
- Short sequence:

```json
{
  "command": {
    "type": "sequence",
    "interrupt": true,
    "steps": [
      { "type": "face", "expression": "thinking" },
      { "type": "speak", "text": "我已经准备好了。" },
      { "type": "face", "expression": "happy_squint" }
    ]
  }
}
```

For the render fallback smoke test, send a stack-chan envelope through the OpenAI-compatible chat path with `render.target: "xiaopai"` and make the assistant return final text without an explicit `xiaopaiControl.execute` tool call. Expected behavior:

- Xiaopai speaks the final assistant text through a fallback `sequence`.
- The normal chat response remains unchanged.
- Plugin logs include `fallback_rendered` with the stack-chan `event_id` when provided.

Repeat with an explicit successful `xiaopaiControl.execute` `speak` or `sequence` containing `speak`; expected behavior is no duplicate fallback and a log outcome of `explicit_rendered`.
