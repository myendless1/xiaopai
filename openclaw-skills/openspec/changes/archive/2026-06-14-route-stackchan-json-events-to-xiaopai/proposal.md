## Why

The current stack-chan integration still forwards OpenClaw-bound robot events as natural-language text. This makes the OpenClaw path less deterministic than the intended boundary: stack-chan should report eligible events as structured input, OpenClaw should decide the assistant response, and `xiaopai-control` should render that response on Xiaopai. The existing `head_touch`/`touch` local shortcut remains an explicit immediate-feedback exception.

## What Changes

- Change stack-chan's OpenClaw forwarding payload from prose event descriptions to a fixed JSON event envelope that contains a work-assistant-compatible `InputEvent`.
- Stop treating stack-chan event forwarding as a place for business or presentation decisions; forwarded robot events should not enqueue local Xiaopai speech, face, or motion commands by themselves. `head_touch`/`touch` remains locally handled and is not forwarded.
- Update the `work-assistant` companion skill so OpenClaw agents recognize stack-chan JSON event envelopes, inspect the embedded `InputEvent`, and call `workAssistant.handleEvent` only when the event matches work-assistant's business capabilities.
- Update the `xiaopai-control` companion skill so OpenClaw agents render robot responses through `xiaopaiControl.execute`, whether the response came from `workAssistant.handleEvent` or was generated directly by the agent, preferring a single validated `sequence`.
- Keep the existing plugin Gateway methods and contracts: do not add a new plugin method, do not make `work-assistant` depend on Xiaopai, and do not add model-backed command planning inside `xiaopai-control`.

## Capabilities

### New Capabilities

- `stackchan-openclaw-xiaopai-routing`: Defines the structured stack-chan-to-OpenClaw event envelope and the required OpenClaw agent routing from `InputEvent` to `StructuredResponse` to Xiaopai command execution.

### Modified Capabilities

- None.

## Impact

- Affected external service: `/home/ubuntu/stack-chan/stack-chan-server/src/server.py` and its command/API documentation.
- Affected OpenClaw guidance: `plugins/work-assistant/skills/work-assistant/SKILL.md` and `plugins/xiaopai-control/skills/xiaopai-control/SKILL.md`.
- Affected runtime path: stack-chan OpenAI-compatible `/chat/completions` calls to the local OpenClaw Gateway.
- No new OpenClaw Gateway methods, plugin dependencies, or unauthenticated network surfaces are introduced.
