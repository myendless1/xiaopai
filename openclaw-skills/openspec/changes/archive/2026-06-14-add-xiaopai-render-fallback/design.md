## Context

Stack-chan sends Xiaopai speech and device input to OpenClaw through the OpenAI-compatible `/chat/completions` path. The user message content is a compact JSON envelope with `schema: "openclaw.stackchan.event.v1"` and, for speech recognition, an embedded `event.type: "user_utterance"`. The envelope also carries `render: { "target": "xiaopai", "interrupt": true }`.

The current contract relies on prompt and companion skill guidance: the agent is told to call `xiaopaiControl.execute` whenever a stack-chan user utterance needs a robot response. That is not a hard runtime guarantee. If the agent returns final assistant text without calling the plugin, OpenClaw appears to answer in chat while the physical Xiaopai remains silent.

`xiaopai-control` already provides the correct execution boundary. It validates commands, supports `speak` and `sequence`, applies a configured default device id when needed, and returns stable queued/rejected/failed results. The missing piece is a turn-level guardrail in OpenClaw orchestration that notices required Xiaopai rendering was skipped.

## Goals / Non-Goals

**Goals:**

- Guarantee a physical Xiaopai speech render for stack-chan turns that explicitly request `render.target: "xiaopai"` and produce user-facing final text.
- Prefer explicit agent calls to `xiaopaiControl.execute`; run fallback only when the agent failed to queue speech for the turn.
- Preserve the stack-chan envelope, session key, and command queue boundaries already introduced by `route-stackchan-json-events-to-xiaopai`.
- Use the existing `xiaopaiControl.execute` Gateway method and command validation.
- Make fallback outcomes observable for debugging: rendered explicitly, rendered by fallback, skipped with reason, or failed.

**Non-Goals:**

- Do not make stack-chan parse OpenClaw responses or synthesize TTS from returned text.
- Do not add robot control to `work-assistant`.
- Do not add a new Gateway method or bypass `xiaopai-control` validation.
- Do not fallback-render non-Xiaopai turns, debug/tool-only responses, or turns with no user-facing final text.
- Do not solve all multimodal presentation planning; this change only guarantees a safe speech fallback.

## Decisions

### Detect Xiaopai render intent from the incoming envelope

The runtime should inspect each chat-completions turn before or during agent execution. If a user message parses as JSON with `schema: "openclaw.stackchan.event.v1"`, an `event` object, and `render.target` equal to `xiaopai`, the turn is marked as Xiaopai-render-required.

The render context should preserve:

- `device_id` from the outer envelope, falling back to `event.payload.device_id` if needed.
- `interrupt` from `envelope.render.interrupt`, defaulting to `true` when omitted.
- `event_id` and `event.type` for logging and duplicate diagnosis.

Alternative considered: infer render intent from model prompt text or session prefix. Rejected because the envelope is the explicit integration contract and avoids accidental robot output for unrelated chat sessions.

### Observe speech-capable Xiaopai tool calls during the turn

The runtime should observe Gateway calls made by the agent in the current turn and mark the turn as speech-rendered only when `xiaopaiControl.execute` successfully queues either:

- a `speak` command with non-empty `text`, or
- a `sequence` command containing at least one `speak` step with non-empty `text`.

Face, action, move, health, device-list, rejected, and failed calls do not count as speech-rendered. They may still be useful explicit presentation, but they do not speak the final text.

Alternative considered: treat any `xiaopaiControl.execute` call as rendered. Rejected because the failure mode is silent text responses; an action-only command still leaves the spoken answer missing.

### Run fallback after final text is known, before returning the response

The fallback should run at the end of the agent turn, after tool execution and final text generation, but before the chat-completions response is returned to stack-chan. If the turn is Xiaopai-render-required, has user-facing final text, and has no successful speech-capable Xiaopai call, OpenClaw should call `xiaopaiControl.execute` itself.

The fallback command should be a validated `sequence`:

```json
{
  "command": {
    "type": "sequence",
    "device_id": "<envelope.device_id>",
    "interrupt": true,
    "steps": [
      { "type": "speak", "text": "<final assistant text>" },
      { "type": "face", "expression": "calm" }
    ]
  }
}
```

If no device id is available, the fallback may omit `device_id` and let the plugin use its configured default or stack-chan's first-online-device behavior. The final text must be normalized before execution: strip transport metadata, tool diagnostics, and raw JSON; trim whitespace; and keep the text within `xiaopai-control`'s current `MAX_SPEECH_LENGTH` of 500 characters by summarizing or truncating conservatively.

Alternative considered: have stack-chan parse the returned assistant text and call `/command/speak` directly. Rejected because stack-chan would regain presentation ownership, duplicate explicit plugin calls, and bypass the OpenClaw plugin validation/audit boundary.

### Make fallback observable without changing the public response shape first

The implementation should record structured logs or internal metadata for:

- `explicit_rendered`: a speech-capable `xiaopaiControl.execute` call succeeded.
- `fallback_rendered`: fallback queued a speech command.
- `fallback_skipped`: fallback did not run, with a reason such as `not_stackchan`, `not_xiaopai_target`, `no_final_text`, or `already_rendered`.
- `fallback_failed`: fallback attempted to call `xiaopaiControl.execute` but the plugin rejected or failed the command.

The initial implementation can keep the external OpenAI-compatible response shape unchanged. If the runtime already supports response metadata, it may include these fields for diagnostics as long as clients that expect normal chat completions are not broken.

Alternative considered: require stack-chan to inspect a new response metadata field. Rejected for this change because stack-chan currently treats the OpenClaw response as opaque and the reliability issue is inside OpenClaw's physical-render obligation.

## Risks / Trade-offs

- Fallback could duplicate speech if the agent queues speech through an unobserved path -> Observe the canonical Gateway method `xiaopaiControl.execute` and document that direct HTTP robot calls are outside the reliable render contract.
- Fallback could speak internal/debug text -> Use only the final user-facing assistant message, skip tool-only/debug responses, and strip raw JSON or transport diagnostics before speaking.
- Fallback could speak text that is too long for `xiaopai-control` validation -> normalize to 500 characters or fewer before calling the plugin.
- Plugin unavailable or lacking permission still causes silence -> surface `fallback_failed` with the plugin result/error so deployment can distinguish model noncompliance from control-plane failure.
- Runtime hook location may differ across OpenClaw versions -> implement as a small turn-level render guard around the existing chat-completions agent execution rather than embedding logic in business plugins.

## Migration Plan

1. Locate the OpenClaw chat-completions turn orchestration point that has access to incoming messages, tool/Gateway call results, and final assistant text.
2. Add stack-chan envelope detection and a per-turn Xiaopai render context.
3. Add observation of successful speech-capable `xiaopaiControl.execute` calls during the turn.
4. Add end-of-turn fallback execution through the existing Gateway method.
5. Add unit tests for explicit render, fallback render, skip reasons, command construction, length handling, and plugin failure reporting.
6. Verify with dry-run `xiaopai-control`, then with a real stack-chan server and device.

Rollback is to disable the render guard or feature flag. Existing explicit agent calls to `xiaopaiControl.execute` continue to work because the plugin contract is unchanged.

## Open Questions

- Should fallback be controlled by a runtime feature flag per model/session, or enabled unconditionally for all `render.target: "xiaopai"` stack-chan envelopes?
- If the final answer exceeds 500 characters, should the first implementation truncate deterministically or ask the model/runtime summarizer for a shorter spoken variant?
- Should action-only explicit commands suppress fallback for presentation-only intents where speaking the final text may be unnecessary, or is speech always required when final user-facing text exists?
