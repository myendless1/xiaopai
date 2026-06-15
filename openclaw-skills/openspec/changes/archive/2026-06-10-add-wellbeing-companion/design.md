## Context

The `work-assistant` plugin currently exposes one Gateway method, `workAssistant.handleEvent`, and routes supported `InputEvent.type` values to domain handlers. Calendar creation handles user-triggered write operations, while agenda briefing handles read-only robot/system triggers and already introduced reusable calendar list infrastructure.

The wellbeing companion scene is different from calendar creation because the primary trigger is not a free-form user request. A robot or sensing service detects sedentary behavior and sends a fixed-format event or fixed-format message. OpenClaw should normalize that input, decide whether interrupting is appropriate, optionally enrich the response with nearby calendar context, and return speech plus presentation hints. OpenClaw should not perform posture detection, camera capture, robot hardware control, or long-term health monitoring.

## Goals / Non-Goals

**Goals:**

- Add a `WellbeingCompanionAssistant` domain handler for sedentary-care events.
- Support `sedentary_detected` as a first-class system event through `workAssistant.handleEvent`.
- Support a follow-up companionship turn when the user accepts the offer for a short joke, relaxation prompt, or light chat.
- Validate sedentary event inputs before generating a nudge.
- Apply deterministic interruption rules using duration, confidence, nearby calendar state, and recent reminder context.
- Reuse the existing calendar list adapter to detect meetings in progress and upcoming reminders.
- Return bounded, robot-speakable `StructuredResponse` output with action records and context patches.
- Update companion skill guidance so fixed-format robot messages are converted into structured `InputEvent` calls.

**Non-Goals:**

- Do not implement camera/photo capture, posture recognition, or sensing logic in the plugin.
- Do not directly control robot display, motion, lights, or speech hardware.
- Do not add a new Gateway method or OpenClaw core transport.
- Do not add Lark IM messaging, weather, route planning, or travel reminder behavior in this change.
- Do not add persistent long-term wellbeing history or health analytics.
- Do not use model-backed joke generation or model-backed interruption decisions inside the plugin.

## Decisions

### Route wellbeing events through the existing handler

`createWorkAssistantHandler` will dispatch by event type:

- `user_utterance` continues to route to `CalendarAssistant` unless a later structured intent router supersedes that path.
- `head_touch` and `daily_briefing_triggered` continue to route to `AgendaBriefingAssistant`.
- `sedentary_detected` routes to `WellbeingCompanionAssistant`.
- A follow-up event such as `wellbeing_companion_requested` routes to `WellbeingCompanionAssistant` instead of `CalendarAssistant`.

Alternative considered: add a new Gateway method such as `workAssistant.handleWellbeing`. Rejected because the existing contract is already transport-independent and event-driven; adding another method would duplicate routing and complicate robot integration.

### Keep robot perception outside the plugin

The plugin will accept sensing outputs, not raw sensor data. The expected payload is a compact structure such as:

```json
{
  "duration_minutes": 30,
  "confidence": 0.86,
  "source": "robot_vision"
}
```

Optional context fields such as `device_id`, `locale`, and current user state can stay under `context`.

Alternative considered: call a vision or posture service from the plugin. Rejected because the existing architecture assigns robot perception to the robot or sensing service, and the first implementation only needs business decision and response generation.

### Use deterministic interruption rules first

The wellbeing assistant will evaluate a small rule set:

- `duration_minutes` must meet a configurable minimum such as 20 minutes.
- `confidence` must meet a configurable minimum such as 0.8.
- If calendar context indicates the user is currently in a meeting or has a meeting starting immediately, avoid an audible nudge or return a skipped/quiet response.
- If `context.last_wellbeing_nudge_at` or a plugin-held short-term state indicates a recent nudge within a cooldown window, skip duplicate reminders.

The decision should be reported through a `wellbeing.sedentary.evaluate` action with details such as `decision`, `duration_minutes`, `confidence`, and `reason`.

Alternative considered: ask a model whether to interrupt. Rejected because interruption behavior must be predictable, testable, and easy to tune before introducing subjective model decisions.

### Reuse calendar list for near-term context

The assistant will query a bounded local-time window around the event timestamp:

- Current meeting window: enough to detect events that overlap the current time.
- Upcoming reminder window: a short horizon such as the next 30 minutes.

If calendar listing succeeds, the assistant can avoid interrupting during meetings and mention a near-future event when helpful. If calendar listing fails, the assistant should return a degraded wellbeing response rather than failing the whole event.

Alternative considered: require the robot or OpenClaw caller to pass all calendar context in payload. Rejected for the first implementation because the plugin already owns Lark calendar reads and can keep enrichment behavior consistent with agenda briefing.

### Generate bounded template speech

Speech generation will use deterministic templates and a small built-in content bank for the first implementation:

- Initial nudge: ergonomic stretch and eye-relaxation suggestion.
- Offer: ask whether the user wants a short joke or relaxation prompt.
- Follow-up: return one short joke or relaxation prompt.
- Optional calendar note: append one concise reminder if a nearby event is selected.

The assistant must keep speech short enough for robot playback and avoid medical claims. Content selection can be deterministic by event id or timestamp to keep tests stable.

Alternative considered: call a model for humor and companionship text. Rejected for this change because the plugin currently avoids model-backed generation and testability is more important than content variety.

### Use context patches for short-term wellbeing state

The response will include `context_patch` fields such as:

- `wellbeing_last_nudge_at`
- `wellbeing_last_decision`
- `wellbeing_follow_up_offered`
- `wellbeing_nearby_event`

Callers that maintain session context can pass these values back on later events. The plugin may also keep an in-memory cooldown helper for runtime tests, but persistent cooldown storage is out of scope.

Alternative considered: add persistent user wellbeing storage immediately. Rejected because the current idempotency store is in-memory and the first capability only needs short-term duplicate suppression.

### Keep fixed-format text normalization in skill guidance

If the robot integration can only send fixed-format content to OpenClaw, the packaged skill should explain how to recognize that format and call `workAssistant.handleEvent` with a structured `InputEvent`. The plugin itself should not grow a broad text parser for robot messages.

Alternative considered: parse fixed-format robot strings inside the plugin. Rejected because OpenClaw is already the layer for normalization and because the plugin should remain an execution boundary with deterministic input validation.

## Risks / Trade-offs

- Sensing events can be noisy -> Require confidence and duration thresholds, and return skipped actions for low-confidence events.
- Users may be interrupted at the wrong time -> Query calendar context and use a cooldown window before speaking.
- Calendar read permissions may be missing -> Record a failed `lark.calendar.list` action and continue with a degraded wellbeing nudge when appropriate.
- Built-in joke content may feel repetitive -> Keep the content bank isolated behind a small helper so a later content service or model can replace it.
- Context patches depend on caller behavior -> Make the assistant useful without returned context, but improve duplicate suppression when context is supplied.
- The active calendar structured-intent change also touches contracts and handler routing -> Implement this change after that work is merged or reconcile both routing changes carefully.

## Migration Plan

1. Add wellbeing event and payload types while preserving the existing `InputEvent` and `StructuredResponse` envelope.
2. Add `WellbeingCompanionAssistant`, deterministic rules, calendar context enrichment, speech templates, and content selection helpers.
3. Extend handler options and plugin factory wiring to instantiate the wellbeing assistant with the existing calendar adapter.
4. Add fixtures and tests for accepted nudges, skipped low-confidence events, cooldown skips, meeting-overlap suppression, degraded calendar reads, nearby-event reminders, and follow-up companionship.
5. Update README and packaged skill guidance with `sedentary_detected` and fixed-format message normalization examples.
6. Verify with `npm run verify`, OpenSpec validation, and dry-run Gateway calls.

Rollback is to remove the wellbeing handler registration and documentation. Calendar creation and agenda briefing remain isolated and should continue to work.

## Open Questions

- What exact fixed-format robot message will OpenClaw receive before normalization, and does it include event id, user id, duration, confidence, and device id?
- Should the first follow-up content support only jokes, or both jokes and relaxation prompts?
- Should meeting-overlap suppression return no speech, a quiet display-only response, or a short non-audible robot hint?
- What cooldown duration should be used for demo and production: 20 minutes, 30 minutes, or configurable per deployment?
