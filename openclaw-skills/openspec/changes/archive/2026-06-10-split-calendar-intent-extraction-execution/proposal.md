## Why

The current calendar assistant relies on a plugin-local regex parser, so small wording changes can prevent otherwise valid calendar requests from executing. OpenClaw should own flexible natural-language understanding and pass a stable structured intent to the plugin, while the plugin remains the deterministic validation and Lark side-effect boundary.

## What Changes

- Add a canonical structured calendar creation intent contract for OpenClaw-generated calendar requests.
- Update `workAssistant.handleEvent` so `InputEvent.payload` can carry a structured calendar intent in addition to the original user text.
- Refactor the calendar assistant to prefer structured intent input, validate it, resolve attendees, and create Lark events without re-parsing the original utterance.
- Keep the current natural-language text parser as a backward-compatible fallback for existing callers and smoke tests.
- Add failure/follow-up behavior for malformed structured intent, unsupported intent versions, missing fields, invalid time ranges, and ambiguous attendee resolution.
- Update companion skill guidance so OpenClaw first extracts a structured intent, then calls the plugin for execution.

## Capabilities

### New Capabilities

- `structured-calendar-intent`: Defines the fixed structured calendar creation intent shape that OpenClaw or other callers provide to the plugin.

### Modified Capabilities

- `work-assistant-event-contract`: `InputEvent.payload` can include structured intent data and still returns the same `StructuredResponse` contract.
- `calendar-assistant`: Calendar creation execution no longer depends on wording-specific parsing when structured intent is present.

## Impact

- Affects `plugins/work-assistant/src/contracts.ts`, `src/calendar/*`, `src/handler.ts`, tests, README, and packaged skill guidance.
- Adds or updates TypeScript runtime validation for structured calendar intent payloads.
- Keeps existing Lark adapter interfaces and `lark-cli` execution behavior unchanged.
- Keeps `workAssistant.handleEvent` as the canonical Gateway method; no OpenClaw core change is required.
- Improves utterance coverage by moving flexible language interpretation to OpenClaw while preserving a deterministic plugin execution boundary.
