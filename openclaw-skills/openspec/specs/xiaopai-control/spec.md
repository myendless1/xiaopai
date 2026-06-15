# xiaopai-control

## Purpose
Defines the OpenClaw plugin boundary for validated Xiaopai / stack-chan command execution and status checks.

## Requirements

### Requirement: Xiaopai plugin gateway surface
The system SHALL provide an OpenClaw `xiaopai-control` plugin with Gateway methods for command execution, health checks, and device listing.

#### Scenario: Command execution method is available
- **WHEN** the plugin is installed and inspected through the OpenClaw runtime
- **THEN** the Gateway method `xiaopaiControl.execute` is registered for Xiaopai command execution

#### Scenario: Read-only status methods are available
- **WHEN** the plugin is installed and inspected through the OpenClaw runtime
- **THEN** the Gateway methods `xiaopaiControl.getHealth` and `xiaopaiControl.listDevices` are registered for Xiaopai deployment checks

### Requirement: Xiaopai command validation
The system MUST validate Xiaopai commands before calling the stack-chan HTTP server.

#### Scenario: Speech command requires text
- **WHEN** `xiaopaiControl.execute` receives a `speak` command with missing, empty, or non-string `text`
- **THEN** the system rejects the command without calling the stack-chan server
- **THEN** the response includes a stable validation error code

#### Scenario: Expression command uses allowlisted expression
- **WHEN** `xiaopaiControl.execute` receives a `face` command whose `expression` is not one of the supported Xiaopai expression names
- **THEN** the system rejects the command without calling the stack-chan server
- **THEN** the response identifies the unsupported expression

#### Scenario: Animation command uses allowlisted action
- **WHEN** `xiaopaiControl.execute` receives an `action` command whose `action` is not one of the supported Xiaopai animation names
- **THEN** the system rejects the command without calling the stack-chan server
- **THEN** the response identifies the unsupported action

#### Scenario: Motion command validates direction and bounds
- **WHEN** `xiaopaiControl.execute` receives a `move` command with an unsupported direction, invalid degree, or invalid duration
- **THEN** the system rejects the command without calling the stack-chan server
- **THEN** the response includes a validation error describing the invalid field

#### Scenario: Center motion does not require degree
- **WHEN** `xiaopaiControl.execute` receives a `move` command with direction `center` and no degree
- **THEN** the system accepts the command as valid

### Requirement: Xiaopai command execution
The system SHALL translate valid Xiaopai commands to stack-chan server HTTP requests and return normalized command results.

#### Scenario: Speech command is queued
- **WHEN** `xiaopaiControl.execute` receives a valid `speak` command
- **THEN** the system sends a stack-chan command equivalent to `POST /command` with type `speak`
- **THEN** the response reports a successful `xiaopai.command` action with command type `speak`

#### Scenario: Face expression command is queued
- **WHEN** `xiaopaiControl.execute` receives a valid `face` command
- **THEN** the system sends a stack-chan command equivalent to `POST /command` with type `face`
- **THEN** the response reports a successful `xiaopai.command` action with command type `face`

#### Scenario: Head motion command is queued
- **WHEN** `xiaopaiControl.execute` receives a valid `move` command
- **THEN** the system sends a stack-chan command equivalent to `POST /command` with type `move`
- **THEN** the response reports a successful `xiaopai.command` action with command type `move`

#### Scenario: Stop command stops playback
- **WHEN** `xiaopaiControl.execute` receives a valid `stop` command
- **THEN** the system calls the stack-chan stop endpoint
- **THEN** the response reports a successful `xiaopai.command` action with command type `stop`

#### Scenario: Adapter failure is normalized
- **WHEN** the stack-chan server request fails, times out, or returns an invalid response
- **THEN** the system returns a failed `xiaopai.command` action
- **THEN** the response includes a sanitized error code and message

### Requirement: Xiaopai sequence execution
The system SHALL support queued command sequences for multi-step robot presentation.

#### Scenario: Valid sequence is queued
- **WHEN** `xiaopaiControl.execute` receives a `sequence` command containing valid `face`, `action`, `move`, and `speak` steps
- **THEN** the system sends a stack-chan command equivalent to `POST /command` with type `sequence`
- **THEN** the response reports a successful `xiaopai.command` action with command type `sequence`

#### Scenario: Invalid sequence step is rejected
- **WHEN** a `sequence` command contains any invalid step
- **THEN** the system rejects the entire sequence without calling the stack-chan server
- **THEN** the response identifies the invalid step index or field

### Requirement: Xiaopai status operations
The system SHALL expose normalized health and device-list operations for the stack-chan server.

#### Scenario: Health check returns normalized status
- **WHEN** `xiaopaiControl.getHealth` is called
- **THEN** the system calls the stack-chan health endpoint
- **THEN** the response indicates whether the stack-chan server is reachable

#### Scenario: Device list returns online devices
- **WHEN** `xiaopaiControl.listDevices` is called
- **THEN** the system calls the stack-chan device-list endpoint
- **THEN** the response includes online device identifiers and available command queue metadata when provided by the stack-chan server

### Requirement: Xiaopai plugin configuration
The system SHALL allow deployments to configure stack-chan connection behavior without code changes.

#### Scenario: Default configuration targets local server
- **WHEN** no explicit base URL is configured
- **THEN** the plugin uses `http://127.0.0.1:8091` as the stack-chan server base URL

#### Scenario: Default device id is optional
- **WHEN** no `device_id` is provided in a command and no default device id is configured
- **THEN** the plugin omits `device_id` and allows the stack-chan server to target the first currently online Xiaopai device

#### Scenario: Dry-run avoids network calls
- **WHEN** the plugin is configured with `dryRun` enabled
- **THEN** `xiaopaiControl.execute`, `xiaopaiControl.getHealth`, and `xiaopaiControl.listDevices` return deterministic dry-run responses
- **THEN** the system performs no network calls to the stack-chan server

### Requirement: Xiaopai companion skill guidance
The system SHALL package companion skill guidance that teaches OpenClaw agents how to use the Xiaopai control plugin.

#### Scenario: Agent renders structured response through Xiaopai
- **WHEN** an OpenClaw agent has a `StructuredResponse` containing `speech` and supported presentation hints
- **THEN** the companion guidance instructs the agent to call `xiaopaiControl.execute` with a `sequence` command that combines expression, motion, and speech steps

#### Scenario: MCP is described as out of scope
- **WHEN** the companion guidance describes external tool integration options
- **THEN** it states that MCP is not part of the MVP and should be added later only as an adapter over the same command contract
