## Context

The current `work-assistant` plugin owns both natural-language parsing and Lark calendar side effects. This worked for the MVP utterance pattern, but it makes execution brittle: equivalent requests such as "明天上午10点到10点半有个活动 X" can fail because the regex parser expects a narrower wording.

OpenClaw is the better layer for flexible language understanding. The plugin should receive a deterministic intent object, validate it, resolve Lark attendees, execute the write, and return `StructuredResponse`.

## Goals / Non-Goals

**Goals:**

- Add a stable structured calendar creation intent shape.
- Allow `workAssistant.handleEvent` callers to submit that structured intent through `InputEvent.payload`.
- Refactor calendar execution so structured intent bypasses wording-specific parsing.
- Keep current text-based parsing as a compatibility fallback.
- Validate structured intent before Lark side effects and return follow-up responses for incomplete or invalid data.
- Update tests and skill guidance so OpenClaw extracts first and the plugin executes second.

**Non-Goals:**

- Do not implement model prompting inside the plugin.
- Do not add a new Gateway method or transport in this change.
- Do not change Lark adapter behavior beyond the input it receives.
- Do not implement calendar update, meeting room selection, free/busy conflict detection, recurrence, or deletion.
- Do not make idempotency persistent in this change.

## Decisions

### Structured intent lives in `payload.structured_intent`

Callers will submit:

```json
{
  "type": "calendar.create",
  "version": "1",
  "title": "活动 XXX",
  "start": "2026-06-06T10:00:00+08:00",
  "end": "2026-06-06T10:30:00+08:00",
  "attendees": [
    { "name": "Gargantua" }
  ]
}
```

The original `payload.text` can remain present for audit/debug context, but it is not used for title/time extraction when a valid structured intent exists.

Alternative considered: replace `payload.text` entirely. Rejected because existing callers and tests already use text-only events, and keeping text helps debugging without changing the top-level `InputEvent` contract.

### Plugin validates, but does not infer missing structured fields

Structured intent validation will require title, start, end, and at least one attendee identifier or name. Time values must be ISO timestamps, and end must be after start.

Alternative considered: let the plugin infer missing fields from text if a partial structured intent is present. Rejected because mixed inference makes behavior harder to reason about and can reintroduce wording brittleness. Partial structured intents should return a follow-up.

### Attendees support names first, identifiers later

For this change, structured attendees will support `{ "name": "..." }` and may also support `{ "id": "ou_xxx" }` as an execution-ready path. Names continue through the existing contact adapter. IDs can bypass contact lookup when they are valid Lark attendee IDs.

Alternative considered: require OpenClaw to resolve attendees before calling the plugin. Rejected for now because Lark contact resolution is already plugin-owned, tested, and dependent on the same Lark user identity used for calendar writes.

### Current parser remains as fallback

If `payload.structured_intent` is absent, the plugin uses the current `CalendarIntentParser` against `payload.text`. This preserves current behavior and allows staged adoption by the companion skill and other callers.

Alternative considered: remove text parsing immediately. Rejected because it would break existing fixture behavior and make rollout unnecessarily risky.

### Keep adapter interfaces stable

`LarkContactAdapter` and `LarkCalendarAdapter` stay unchanged. The new structured intent is normalized into the existing internal create request before adapter calls.

Alternative considered: pass the new payload object into adapters directly. Rejected because adapters should remain transport-specific Lark boundaries, not assistant-intent interpreters.

## Risks / Trade-offs

- Structured intent producers may send malformed objects -> Add runtime validation and explicit follow-up reasons such as `invalid_structured_intent`.
- Version drift between OpenClaw extraction and plugin execution -> Include a `version` field and reject unsupported versions without side effects.
- Existing text parser remains incomplete -> Treat it as fallback only and update skill guidance to prefer structured intent.
- Attendee IDs bypass contact lookup incorrectly -> Accept only known Lark attendee ID prefixes (`ou_`, `oc_`, `omm_`) and otherwise require name resolution.
- In-memory idempotency still resets on Gateway restart -> Document as existing behavior; leave persistent idempotency for a future change.

## Migration Plan

1. Add structured intent types and validation helpers.
2. Update calendar assistant to derive its internal `CalendarCreateIntent` from structured intent when present.
3. Preserve text parser fallback behavior and existing tests.
4. Add tests for structured success, malformed structured intent, unsupported version, invalid time range, name attendee resolution, ID attendee bypass, and duplicate event id behavior.
5. Update README and packaged skill guidance with the new preferred call shape.
6. Build, test, reinstall/reload the plugin, verify dry-run, then verify real Lark creation behind explicit operator confirmation.

Rollback is to remove `payload.structured_intent` usage from callers; text-only behavior remains available.

## Open Questions

- Should structured extraction be done by a companion skill prompt only, or should a later OpenClaw core/workflow layer provide a reusable structured extraction helper?
- Should attendee display names and IDs both be stored in `context_patch` for later edit/delete flows?
- Should the plugin eventually require structured intent for all write operations after a deprecation period?
