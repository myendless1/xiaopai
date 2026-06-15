## Why

OpenClaw currently returns robot-facing speech and presentation hints, but there is no stable OpenClaw execution boundary that can render those results on the local Xiaopai device. The stack-chan server already exposes a local HTTP API for speech, expressions, head motion, sequences, devices, and health checks; wrapping it as an OpenClaw plugin gives agents a validated, testable tool surface instead of direct ad hoc `curl` calls.

## What Changes

- Add a new `xiaopai-control` OpenClaw plugin as the execution boundary for controlling the Xiaopai / stack-chan device.
- Expose a Gateway method for queued device commands covering speech, face expressions, face animations, head motion, action sequences, and stop.
- Expose read-only Gateway methods for health checks and online device listing.
- Add plugin configuration for the stack-chan server base URL, optional default device id, request timeout, and dry-run behavior.
- Validate supported command types, expression names, animation names, movement directions, numeric bounds, and sequence steps before calling the stack-chan server.
- Return normalized command results and action records that OpenClaw callers can inspect without depending on raw stack-chan HTTP response shapes.
- Package a companion OpenClaw skill that explains when and how agents should call the plugin, including how to render existing `StructuredResponse.speech` and `StructuredResponse.presentation` hints.
- Keep MCP out of the MVP; if external MCP clients need the same capability later, add an adapter that forwards the same validated command contract into the plugin.

## Capabilities

### New Capabilities

- `xiaopai-control`: Defines the OpenClaw plugin contract for validating, queuing, and reporting Xiaopai speech, expression, animation, head-motion, sequence, stop, health, and device-list operations through the existing stack-chan HTTP server.

### Modified Capabilities

- None.

## Impact

- Adds a new plugin package under `plugins/xiaopai-control`.
- Adds an `openclaw.plugin.json` manifest with Gateway methods and plugin configuration schema.
- Adds TypeScript command contracts, runtime validation, stack-chan HTTP adapter, dry-run adapter, handler methods, tests, fixtures, and README/verification docs.
- Adds a companion skill under the plugin so OpenClaw agents can choose this tool for robot presentation and hardware rendering.
- Depends on an already running stack-chan server, defaulting to `http://127.0.0.1:8091`, and the device being online through its existing polling loop.
- Does not change `work-assistant` requirements or make OpenClaw core changes.
