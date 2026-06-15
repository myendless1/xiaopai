## Why

The proactive scheduler can already detect `outdoor_event_detected` and `business_trip_tomorrow_detected` events, but Work Assistant currently has no domain handler that turns those triggers into useful travel guidance. This leaves the afternoon outdoor-visit and pre-trip reminder scenes from the top-level requirements unimplemented even though the event infrastructure is in place.

## What Changes

- Add a `travel-planner` Work Assistant capability for proactive same-day outdoor visit reminders and next-day business trip reminders.
- Route scheduler-produced `outdoor_event_detected` and `business_trip_tomorrow_detected` events to a new travel planner domain assistant.
- Add route, weather, and user-profile adapter contracts with deterministic dry-run implementations, so the business flow can be tested without binding the MVP to a specific map or weather provider.
- Generate bounded robot-speakable guidance that includes destination, event time, recommended departure time when route data is available, weather notes when forecast data is available, and carry/preparation reminders from event metadata.
- Record route lookup, weather lookup, and travel plan generation as structured action records, including degraded responses when external data is missing or adapters fail.
- Add plugin configuration for origin address, arrival buffer, route mode, and dry-run travel data.
- Update README, packaged skill guidance, fixtures, and verification notes for outdoor and business-trip travel reminder flows.
- Keep complex travel booking, multi-leg itinerary planning, live traffic prediction, and real provider integrations outside this first change unless implemented behind the same adapter contracts.

## Capabilities

### New Capabilities

- `travel-planner`: Defines proactive outdoor-visit and business-trip reminder behavior, route/weather/profile adapter expectations, degraded travel guidance, action reporting, and dry-run verification.

### Modified Capabilities

- `work-assistant-event-contract`: Treat `outdoor_event_detected` and `business_trip_tomorrow_detected` as supported Work Assistant events with transport-independent structured responses and action records.

## Impact

- Affected code: `plugins/work-assistant/src` handler routing, a new travel planner domain module, adapter interfaces and dry-run implementations, runtime config parsing, tests, fixtures, README, packaged skill guidance, and verification notes.
- APIs: preserves the existing `workAssistant.handleEvent` Gateway method and `StructuredResponse` contract; no new public Gateway method is required.
- Scheduler: reuses existing `outdoor_event` and `business_trip_tomorrow` trigger rules and their normalized `payload.calendar_event` summaries.
- Dependencies: introduces internal adapter contracts for route estimates, weather forecasts, and user profile data; real map/weather provider adapters can be added later without changing the domain contract.
- Robot rendering: continues to return presentation hints only; Xiaopai physical output remains owned by `xiaopai-control` or the OpenClaw agent rendering path.
