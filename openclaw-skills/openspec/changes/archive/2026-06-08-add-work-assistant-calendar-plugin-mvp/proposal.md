## Why

The current workplace assistant requirements describe a full-day robot demo, but the system needs a reusable OpenClaw capability boundary instead of six hard-coded demo scripts. A calendar creation assistant is the smallest valuable vertical slice because it is user-triggered, has clear validation rules, produces a real Lark calendar side effect, and establishes the shared event and structured response contract needed by later assistant abilities.

## What Changes

- Add an OpenClaw `work-assistant` plugin as the primary execution boundary for workplace assistant behavior.
- Define a normalized workplace assistant `InputEvent` contract for robot/user events, starting with `user_utterance`.
- Define a `StructuredResponse` contract for robot-consumable speech, presentation hints, executed actions, follow-up prompts, and context patches.
- Add a `calendar_assistant` domain handler that parses natural-language calendar creation requests, validates required fields, resolves Lark attendees, creates Lark calendar events, and returns structured results.
- Add Lark adapter boundaries for contact resolution and calendar event creation/invitation.
- Allow the plugin to ship an optional companion OpenClaw skill that teaches the agent when and how to use the assistant, while keeping execution and side effects in the plugin.
- Exclude MCP from the MVP; an MCP adapter can be added later if external MCP clients need this capability.

## Capabilities

### New Capabilities

- `work-assistant-event-contract`: Normalized input event and structured response contracts between robot-facing callers and OpenClaw workplace assistant capabilities.
- `calendar-assistant`: Calendar creation assistant behavior, including utterance parsing, validation, Lark attendee resolution, event creation, follow-up handling, and action reporting.

### Modified Capabilities

- None.

## Impact

- Adds a new OpenClaw plugin package or plugin module for `work-assistant`.
- Adds plugin-owned route or Gateway method surface for submitting assistant events and receiving structured responses.
- Adds adapter interfaces and implementations for Lark calendar and contact operations.
- May add a companion OpenClaw skill packaged with the plugin for agent guidance, but not as the core execution layer.
- Depends on existing OpenClaw Gateway/plugin infrastructure and available Lark credentials/scopes for calendar and contact operations.
- Provides the base protocol that later proposals can reuse for agenda briefing, meeting reminders, travel planning, and wellbeing companion behavior.
