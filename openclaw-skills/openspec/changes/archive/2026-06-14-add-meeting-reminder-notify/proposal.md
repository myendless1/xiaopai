## Why

The proactive calendar scheduler can already produce `meeting_starting_soon` events, but Work Assistant currently treats those events as unsupported because no meeting reminder handler exists. Implementing meeting reminders next turns the scheduler from infrastructure into a user-visible proactive assistant flow while keeping the scope smaller than travel planning.

## What Changes

- Add a meeting reminder and notification capability for scheduler-produced `meeting_starting_soon` events.
- Route `meeting_starting_soon` through `workAssistant.handleEvent` to a new domain assistant.
- Generate concise robot-speakable meeting reminder responses from `payload.calendar_event` and scheduler trigger metadata.
- Record the reminded meeting in `context_patch.current_focus` so follow-up utterances can refer to "this meeting" or "the attendees".
- Support follow-up user utterances such as "I will be five minutes late, notify them" by resolving the focused meeting and sending a Lark IM notification through an adapter.
- Add dry-run IM behavior, fixtures, tests, and documentation for reminder and notification flows.
- Keep route, weather, travel, and general meeting minutes behavior out of this change.

## Capabilities

### New Capabilities

- `meeting-reminder-notify`: Handles proactive meeting-starting-soon reminders, meeting focus context, and user-requested late-arrival notifications to meeting participants.

### Modified Capabilities

- `work-assistant-event-contract`: Add supported routing and side-effect safety expectations for `meeting_starting_soon` events and follow-up notification requests.

## Impact

- Affected code: `plugins/work-assistant/src` handler routing, a new meeting reminder domain module, Lark adapter interfaces and dry-run implementations, tests, fixtures, README, packaged skill guidance, and verification notes.
- APIs: preserves the existing `workAssistant.handleEvent` Gateway method and `StructuredResponse` contract; no new public Gateway method is required.
- Dependencies: may add a Lark IM adapter backed by `lark-cli` for message sending; production notification behavior depends on appropriate Lark IM permissions and a reliable way to address attendees or a meeting chat.
- Scheduler: reuses the existing `meeting_starting_soon` trigger type and metadata; this change does not require new scheduler scan behavior.
