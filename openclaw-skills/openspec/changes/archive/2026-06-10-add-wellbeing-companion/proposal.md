## Why

The workplace assistant requirements include a sedentary-care and light-companionship scene, but the current `work-assistant` plugin only handles calendar creation and agenda briefing. Adding this capability lets OpenClaw respond to robot or sensing-service sedentary events with structured, robot-consumable wellbeing guidance while preserving the existing boundary that robot perception and hardware control stay outside OpenClaw.

## What Changes

- Add a `wellbeing_companion` domain capability to the existing `work-assistant` plugin.
- Route `sedentary_detected` system events through `workAssistant.handleEvent` using the existing `InputEvent` envelope.
- Validate sedentary event payload fields such as duration and confidence before generating a nudge.
- Decide whether to interrupt the user based on configurable thresholds, recent reminder state, and calendar context.
- Query near-future calendar events when available so the response can include useful upcoming-event reminders.
- Generate structured wellbeing responses with speech, presentation hints, action records, follow-up state, and context patches.
- Support a follow-up companionship turn, such as a user accepting the offer to hear a short joke or relaxation prompt, without routing that turn to calendar creation.
- Update the packaged companion skill guidance so fixed-format robot messages can be normalized into `InputEvent` calls.
- Do not implement robot camera capture, posture recognition, hardware control, model-backed joke generation inside the plugin, or persistent long-term health tracking.

## Capabilities

### New Capabilities

- `wellbeing-companion`: Handles sedentary detection events, interruption decisions, wellbeing nudge generation, optional light-companionship follow-ups, and nearby calendar reminders.

### Modified Capabilities

- `work-assistant-event-contract`: Adds supported system/user event handling for wellbeing events while keeping the same `InputEvent`, `StructuredResponse`, action reporting, and transport-independent Gateway contract.

## Impact

- Affected plugin code: `plugins/work-assistant/src/handler.ts`, `src/contracts.ts`, `src/index.ts`, a new wellbeing domain module, tests, fixtures, README, and packaged skill guidance.
- Reuses existing Lark calendar list adapter for nearby event context; no new Lark write scope is required for the first implementation.
- Keeps `workAssistant.handleEvent` as the canonical Gateway method; no OpenClaw core change or new transport surface is required.
- Robot or sensing integrations must provide stable event ids and fixed-format sedentary event data, or send fixed-format text that OpenClaw can normalize before calling the plugin.
