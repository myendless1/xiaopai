## 1. Plugin Scaffolding

- [x] 1.1 Create `plugins/xiaopai-control` with package metadata, TypeScript config, Vitest config, build/test scripts, and package file list matching the existing plugin style.
- [x] 1.2 Add `openclaw.plugin.json` with plugin id `xiaopai-control`, startup activation, Gateway method declarations, companion skill path, and config schema for `baseUrl`, `defaultDeviceId`, `timeoutMs`, and `dryRun`.
- [x] 1.3 Add the plugin entrypoint using `definePluginEntry` and register `xiaopaiControl.execute`, `xiaopaiControl.getHealth`, and `xiaopaiControl.listDevices`.

## 2. Command Contracts And Validation

- [x] 2.1 Define TypeScript command/result types for `speak`, `face`, `action`, `move`, `sequence`, `stop`, health, devices, and normalized action records.
- [x] 2.2 Add fixed allowlists for Xiaopai expressions, animations, and move directions from the stack-chan API document.
- [x] 2.3 Implement runtime validation for command shape, speech text, expression/action names, movement direction, numeric bounds, optional `device_id`, `interrupt`, and sequence steps.
- [x] 2.4 Add validation result helpers that return structured rejected results without throwing transport-level errors.

## 3. Stack-Chan Adapters

- [x] 3.1 Define an adapter interface for command execution, health checks, and device listing.
- [x] 3.2 Implement the HTTP adapter using configured `baseUrl`, `timeoutMs`, JSON `POST /command` for supported commands, and the stack-chan stop endpoint for `stop`.
- [x] 3.3 Normalize stack-chan queue responses into `XiaopaiCommandResult` objects with `xiaopai.command` action records.
- [x] 3.4 Normalize network, timeout, non-2xx, and malformed-response failures into failed action records with sanitized error details.
- [x] 3.5 Implement a deterministic dry-run adapter that performs no network calls and returns queued-like health, device, and command results.

## 4. Handler And Gateway Behavior

- [x] 4.1 Implement the `xiaopaiControl.execute` handler so it accepts either a command object directly or `{ "command": <command> }`.
- [x] 4.2 Apply `defaultDeviceId` when a command omits `device_id`, while preserving omission when no default is configured.
- [x] 4.3 Ensure invalid commands are rejected before adapter invocation.
- [x] 4.4 Implement `xiaopaiControl.getHealth` and `xiaopaiControl.listDevices` through the same configured adapter.
- [x] 4.5 Register write scope for command execution and the safest available read scope for health/device methods, documenting any local Gateway limitation.

## 5. Companion Skill And Documentation

- [x] 5.1 Add `skills/xiaopai-control/SKILL.md` explaining when to call the plugin and how to build `speak`, `face`, `action`, `move`, `sequence`, and `stop` commands.
- [x] 5.2 Document how to render an existing `StructuredResponse` through a Xiaopai `sequence` using `speech`, `presentation.emotion`, and `presentation.motion`.
- [x] 5.3 Document that MCP is out of scope for the MVP and should be added later only as an adapter over the same command contract.
- [x] 5.4 Add a plugin README with configuration examples, Gateway call examples, stack-chan server assumptions, dry-run behavior, and trusted-LAN safety notes.
- [x] 5.5 Add a verification document with dry-run build/test commands, Gateway install/inspect commands, and optional real-device smoke-test calls.

## 6. Tests And Verification

- [x] 6.1 Add unit tests for command validation, including invalid speech, unsupported expressions/actions, movement bounds, center movement without degree, and invalid sequence step reporting.
- [x] 6.2 Add adapter tests for HTTP request translation, response normalization, timeout/failure handling, health checks, and device listing.
- [x] 6.3 Add dry-run tests proving execute, health, and device operations avoid network calls.
- [x] 6.4 Add plugin smoke tests verifying Gateway method registration and configured adapter selection.
- [x] 6.5 Run package build and test scripts for `plugins/xiaopai-control`.
- [x] 6.6 Install or link the plugin into the local OpenClaw environment and verify `openclaw plugins inspect xiaopai-control --runtime --json` reports the expected Gateway methods.
- [x] 6.7 If a stack-chan server and Xiaopai device are available, run real-device smoke tests for health, device list, speech, face, move center, stop, and one short sequence.
