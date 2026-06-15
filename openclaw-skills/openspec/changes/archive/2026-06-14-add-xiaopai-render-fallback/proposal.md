## Why

Stack-chan now forwards Xiaopai speech and device input to OpenClaw as structured JSON and expects OpenClaw to render physical robot output through `xiaopaiControl.execute`. In practice, the agent can still return a normal text answer without calling the control plugin, leaving the physical robot silent even though the user interaction succeeded in chat.

## What Changes

- Add a runtime fallback for stack-chan robot turns whose envelope requests `render.target: "xiaopai"`.
- Track whether the current OpenClaw turn successfully invoked `xiaopaiControl.execute` for the target device before the final assistant response is returned.
- When the turn produces user-facing final text but no Xiaopai render call, automatically queue a safe `xiaopaiControl.execute` `sequence` containing a `speak` step for that final text.
- Preserve explicit agent-rendered Xiaopai commands as the preferred path; the fallback only runs when the agent failed to render a required robot response.
- Record fallback execution, skipped, and failed states in logs or response metadata so silent-turn regressions can be diagnosed.
- Do not make stack-chan parse OpenClaw response text, and do not add direct robot control to `work-assistant`.

## Capabilities

### New Capabilities
- `xiaopai-render-fallback`: Guarantees that stack-chan-originated OpenClaw turns with Xiaopai render intent produce a physical Xiaopai speech command when user-facing text would otherwise be returned without a control-plugin call.

### Modified Capabilities
- None.

## Impact

- Affected OpenClaw behavior: chat-completions or agent turn orchestration must detect stack-chan envelopes, observe tool calls, and perform a final render fallback before returning the response.
- Affected plugin dependency: relies on the existing `xiaopaiControl.execute` Gateway method and its validated `sequence` / `speak` command contract.
- Affected docs/tests: add coverage for fallback rendering, explicit-render skip behavior, non-Xiaopai turns, plugin failure reporting, and guidance explaining that the runtime fallback is the reliability layer behind the existing companion-skill instructions.
- Out of scope: stack-chan server command queue behavior, Xiaopai firmware/TTS, `work-assistant` business logic, and new Gateway methods.
