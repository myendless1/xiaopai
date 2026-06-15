## Context

`work-assistant` already has the shared event contract, proactive calendar scheduler, agenda briefing, calendar creation, meeting reminder/notification, and wellbeing domains. The scheduler can produce `outdoor_event_detected` and `business_trip_tomorrow_detected` events from normalized calendar data, but `createWorkAssistantHandler` does not route those event types to a business domain yet. The top-level capability design expects Work Assistant to turn outdoor visits and next-day trips into route, departure-time, weather, and preparation guidance while keeping robot hardware rendering separate.

The existing implementation style is deterministic domain logic plus adapter boundaries: domain assistants normalize input, call adapters, record `ToolAction` entries, and return `StructuredResponse`. Travel planning should follow that pattern instead of placing map/weather logic in the scheduler or asking the model to infer route details inside the plugin.

## Goals / Non-Goals

**Goals:**

- Add a `TravelPlannerAssistant` that handles scheduler-produced `outdoor_event_detected` and `business_trip_tomorrow_detected` events.
- Generate bounded, robot-speakable outdoor visit reminders with destination, event time, route duration when available, recommended departure time when origin and route data are available, and preparation notes from the calendar event.
- Generate bounded next-day business trip reminders with destination/city, event time, weather forecast when available, and carry/preparation notes from title, location, and description.
- Add `RouteAdapter`, `WeatherAdapter`, and `UserProfileAdapter` contracts with dry-run implementations for local verification.
- Keep external failures non-blocking: route/weather/profile lookup failures should produce degraded guidance and failed action records instead of transport-level errors.
- Keep scheduler activation explicit and reuse existing `outdoorEvent` and `businessTripTomorrow` rules.

**Non-Goals:**

- Do not implement booking, ticketing, hotel lookup, RSVP changes, calendar edits, or Lark message sends for travel reminders.
- Do not implement multi-leg itinerary optimization, real-time traffic prediction, or continuous trip monitoring.
- Do not make a specific map or weather provider mandatory in this change.
- Do not ask an LLM/model inside recurring scheduler scans or inside the deterministic travel planner domain.
- Do not make `work-assistant` directly control Xiaopai hardware; presentation remains a structured hint rendered elsewhere.

## Decisions

### Add a dedicated TravelPlannerAssistant

`TravelPlannerAssistant` should own travel-specific normalization and response generation. `createWorkAssistantHandler` should route:

- `outdoor_event_detected` -> `TravelPlannerAssistant.handleOutdoorEvent`
- `business_trip_tomorrow_detected` -> `TravelPlannerAssistant.handleBusinessTripTomorrow`

Alternative considered: extend `AgendaBriefingAssistant`. Rejected because agenda briefing summarizes calendar data, while travel planning introduces route/weather/profile dependencies and proactive departure calculations.

Alternative considered: add travel logic directly in the scheduler. Rejected because scheduler should remain responsible for deterministic trigger planning, not business response generation or external map/weather calls.

### Treat route, weather, and profile as adapters

The domain should depend on small contracts:

```ts
type RouteEstimateRequest = {
  origin: string;
  destination: string;
  departAt: string;
  mode: "driving" | "transit" | "walking";
};

type WeatherForecastRequest = {
  location: string;
  date: string;
};

type UserProfile = {
  originAddress?: string;
  homeCity?: string;
  defaultRouteMode?: "driving" | "transit" | "walking";
  arrivalBufferMinutes?: number;
};
```

Dry-run implementations should return deterministic route and weather results for fixtures such as `客户园区` and `北京`. Real map/weather providers can be added later behind the same contracts.

Alternative considered: call provider CLIs or HTTP APIs directly from `TravelPlannerAssistant`. Rejected because direct calls would make tests brittle and couple the domain to provider-specific error shapes.

### Resolve destination conservatively

Destination resolution should use existing scheduler payload data and avoid guessing. For outdoor events, prefer:

1. `payload.calendar_event.location`
2. explicit destination-like text in `payload.calendar_event.description`
3. obvious destination text from the title only when a conservative rule matches

For business trips, prefer `location` as destination/city, then description/title trip keywords. If no destination can be resolved, the assistant should return a degraded reminder that names the event and asks the user to check the destination, with a skipped or failed travel action rather than a route/weather call.

Alternative considered: model-backed destination extraction. Rejected for this MVP to keep the plugin deterministic and safe in recurring proactive flows.

### Compute departure time only when safe

For outdoor reminders, recommended departure time requires:

- a resolved destination,
- an origin address from config/profile/context,
- a successful route duration,
- and an arrival buffer.

If any piece is missing, the response should still remind the user about the event and destination but omit the precise departure time. The action details should state which input was missing.

For business-trip reminders, route estimates are optional. The core reminder should focus on weather and preparation. Departure-time guidance can be included only if origin and route data are available and the configured behavior enables it.

### Use bounded preparation guidance

Preparation notes should be deterministic and bounded:

- Always include generic essentials for business trips such as ID, charger, and work materials.
- Include rain/temperature guidance only when weather data is available.
- Include at most a short event-description note, such as materials, dress code, or documents, when it is present in the calendar description.
- Avoid medical/safety claims and avoid long itinerary narration.

### Record action outcomes explicitly

Travel responses should include stable action records:

- `travel.plan.generate` for the final domain decision.
- `route.estimate` when route lookup is attempted.
- `weather.forecast` when weather lookup is attempted.
- `user.profile.read` when profile lookup is attempted.

Failures should be represented as `status: "failed"` or `status: "skipped"` action records with safe error codes and messages. A partial response can still be successful from the user's perspective when enough information remains to produce a useful reminder.

## Risks / Trade-offs

- Route/weather providers are not selected yet -> Use adapter contracts and dry-run implementations first; add real providers behind the same contracts later.
- Calendar location data may be vague or missing -> Degrade to a location-check reminder and record destination resolution failure without guessing.
- Origin address may be unavailable -> Omit exact departure time and tell the user the reminder lacks route timing.
- Weather forecast can fail or be unavailable for a destination -> Keep the trip reminder useful with calendar time, destination, and preparation notes; record failed `weather.forecast`.
- Scheduler can dispatch travel events before the domain is configured -> Keep `outdoorEvent` and `businessTripTomorrow` opt-in and document required travel config.
- Speech can become too long for robot playback -> Cap route/weather/preparation text to concise single-turn reminders.

## Migration Plan

1. Add travel contracts and dry-run adapters without changing public Gateway methods.
2. Add `TravelPlannerAssistant` and route the two scheduler-produced travel event types through `createWorkAssistantHandler`.
3. Add plugin config parsing for origin address, route mode, arrival buffer, and dry-run travel defaults.
4. Add fixtures and tests for outdoor reminders, business trip reminders, missing destination, missing origin, route failure, weather failure, and dry-run scheduler dispatch.
5. Update README, packaged skill guidance, and verification notes.
6. Validate with `npm run verify` in `plugins/work-assistant` and `openspec validate add-travel-planner --strict`.

Rollback is to disable `outdoorEvent` and `businessTripTomorrow` scheduler rules or remove the handler routing. Existing calendar creation, agenda briefing, meeting reminders, wellbeing behavior, and Xiaopai rendering remain unaffected.

## Open Questions

- Which real route/weather providers should be used after dry-run verification?
- Should origin address come only from plugin config in the MVP, or should a later profile adapter read per-user office/home preferences from a persisted store?
- Should business-trip reminders include route/departure calculations by default, or only weather and packing guidance until transportation details are modeled more precisely?
