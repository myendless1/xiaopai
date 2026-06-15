## Context

The repository currently contains requirement and design notes for a workplace robot assistant, but no active OpenSpec changes and no main specs. The installed OpenClaw Gateway is running locally on port 18789, the Feishu channel is configured and running, and the host has `lark-cli` plus Lark calendar/contact skills available.

The product direction is to avoid hard-coding the six demo scenes from the requirement document. OpenClaw should act as the business brain and orchestration layer: it accepts robot/user events, calls business tools, makes decisions, and returns structured speech/presentation/action results that the robot side can render.

This MVP covers the first vertical slice only: natural-language Lark calendar creation.

## Goals / Non-Goals

**Goals:**

- Create a local OpenClaw `work-assistant` plugin as the primary execution boundary.
- Establish reusable `InputEvent` and `StructuredResponse` TypeScript contracts.
- Expose one canonical plugin entry point for assistant event handling.
- Implement `calendar_assistant` for the "create a Lark calendar event and invite attendees" workflow.
- Validate write operations before creating events, and return follow-up prompts instead of guessing.
- Keep Lark calendar/contact access behind adapters so the first implementation can use `lark-cli` while later implementations can call Lark OpenAPI directly.
- Package an optional companion OpenClaw skill only as usage guidance for the plugin.

**Non-Goals:**

- Do not implement the full workday demo in this change.
- Do not implement agenda briefing, meeting reminders, travel planning, wellbeing companion behavior, weather, route lookup, or robot sensing.
- Do not replace or modify the existing Feishu channel plugin.
- Do not build an MCP server or adapter in the MVP.
- Do not directly control robot hardware, lights, screen, or motion; return presentation hints only.
- Do not introduce direct OpenClaw core changes unless the plugin API is missing a required stable extension point.

## Decisions

### Use an OpenClaw plugin as the main boundary

The `work-assistant` package will be a normal OpenClaw plugin using `definePluginEntry`. It will own the assistant contracts, domain router, calendar handler, Lark adapters, tests, and optional packaged skill.

Alternatives considered:

- Skill-only: rejected because skills teach an agent how to use tools but do not provide a stable execution boundary for side effects, event ingress, idempotency, or structured robot responses.
- MCP-first: rejected for the MVP because there is no known external MCP client requirement yet, and adding MCP would introduce another protocol/deployment layer before the core behavior is proven.
- OpenClaw core change: rejected unless necessary because this behavior is product-specific and fits the plugin ownership model.

### Expose a canonical Gateway method first

The plugin will expose a Gateway method such as `workAssistant.handleEvent` that accepts `InputEvent` and returns `StructuredResponse`. This gives robot-facing callers and tests a stable RPC surface through the running Gateway.

An HTTP route can be added later as a thin wrapper around the same handler if the robot integration prefers HTTP. The internal domain handler must not depend on the transport.

### Keep Feishu channel separate from Lark business adapters

The existing Feishu channel receives and sends chat messages. It is not the calendar/contact API layer. The calendar assistant will use dedicated adapter interfaces:

- `LarkContactAdapter.resolvePeople(names, options)`
- `LarkCalendarAdapter.createEvent(request)`

For the MVP, these adapters can execute `lark-cli` with fixed argv arrays, JSON output, strict parsing, timeouts, and no shell interpolation. This reuses the already installed Lark auth/tooling while keeping a clean migration path to direct OpenAPI calls.

### Use strict validation before side effects

Calendar creation is a write operation. The handler must create a Lark event only when the title, date, start time, end time, requester identity, and attendee identities are sufficiently resolved.

When required fields are missing, relative time cannot be resolved, attendees have ambiguous matches, the time range is invalid, or an adapter reports a conflict, the handler must return `follow_up.expected=true` and must not call the calendar creation adapter.

### Resolve relative time from the event, not host state

The parser must resolve phrases such as "tomorrow morning" from `InputEvent.timestamp` and `context.timezone`. This avoids test flakiness and prevents incorrect behavior when host time differs from the user's effective timezone.

### Make side effects observable and idempotent

The handler will treat `event_id` as an idempotency key for side-effecting operations. Replaying the same event must not create duplicate calendar events. The response will include action records such as `lark.calendar.create` with status and resource identifiers or errors.

### Keep natural-language parsing replaceable

The first implementation can use a bounded `CalendarIntentParser` for common Chinese meeting-creation utterances. The parser should produce an intermediate `CalendarCreateIntent` and confidence/ambiguity metadata. Later, this seam can move to a model-backed structured extraction path without changing adapters or external contracts.

## Risks / Trade-offs

- `lark-cli` output or shortcut behavior changes -> Keep all Lark access behind adapters, parse JSON strictly, and cover adapter behavior with mocked process tests.
- Duplicate event creation on retries -> Use `event_id` idempotency storage before executing calendar writes.
- Natural-language parsing is incomplete -> Prefer follow-up prompts over guessing, and keep the parser replaceable.
- Lark permission or visibility limits block contact/calendar operations -> Return failed action records and operator-readable errors without exposing secrets.
- Gateway method is not enough for the robot client -> Add an HTTP route later as a wrapper around the same handler.
- Companion skill could be mistaken for the execution layer -> Document that the skill is optional guidance and all side effects live in the plugin.

## Migration Plan

1. Add the `work-assistant` plugin package locally under this repository.
2. Implement contracts, handler, adapters, and tests without changing OpenClaw core.
3. Install or link the plugin into the local OpenClaw extension directory.
4. Restart the Gateway and verify `openclaw plugins inspect work-assistant --runtime --json`.
5. Exercise `workAssistant.handleEvent` with a dry-run or mocked Lark adapter first.
6. Enable real Lark adapter execution after credentials/scopes are confirmed.

Rollback is to disable or uninstall the `work-assistant` plugin. Existing Feishu channel behavior and existing Lark skills remain independent.

## Open Questions

- Should the robot production integration call the Gateway method directly, or should a later proposal add an HTTP route wrapper?
- Which Lark identity should the calendar adapter use by default: user or bot?
- Should the MVP check free/busy conflicts before creation, or only handle conflicts reported by the create operation?
- Where should plugin idempotency/context state live for the first implementation: plugin runtime store, filesystem store, or an OpenClaw-provided store?
