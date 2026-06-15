## 1. Contracts And Configuration

- [x] 1.1 Add travel planner request/result types for route estimates, weather forecasts, user profile travel preferences, destination summaries, and travel context patches.
- [x] 1.2 Add `RouteAdapter`, `WeatherAdapter`, and `UserProfileAdapter` interfaces with normalized success and failure result shapes.
- [x] 1.3 Extend Work Assistant plugin config and manifest schema with travel settings for origin address, route mode, arrival buffer minutes, and dry-run travel defaults.
- [x] 1.4 Ensure `outdoor_event_detected` and `business_trip_tomorrow_detected` remain valid `InputEvent` types and are treated as supported handler events.

## 2. Travel Planner Domain

- [x] 2.1 Create `TravelPlannerAssistant` with `handleOutdoorEvent` and `handleBusinessTripTomorrow` entrypoints.
- [x] 2.2 Implement scheduler calendar-event normalization for title, start, end, calendar id, location, description, trigger key, and source event id.
- [x] 2.3 Implement conservative destination resolution from location, description, and title with missing-destination degraded responses.
- [x] 2.4 Implement outdoor visit route lookup and recommended departure-time calculation using origin, route duration, event start, and arrival buffer.
- [x] 2.5 Implement outdoor reminder speech, presentation hints, `travel.plan.generate`, `route.estimate`, and travel context patch generation.
- [x] 2.6 Implement business trip weather lookup and bounded carry/preparation guidance from forecast and calendar metadata.
- [x] 2.7 Implement business trip reminder speech, presentation hints, `travel.plan.generate`, `weather.forecast`, and travel context patch generation.
- [x] 2.8 Normalize adapter failures and missing profile/config data into degraded structured responses without throwing transport-level errors.

## 3. Runtime Wiring And Dry Run

- [x] 3.1 Add deterministic dry-run route, weather, and user-profile adapters for known fixture destinations such as `客户园区` and `北京`.
- [x] 3.2 Wire travel adapters and `TravelPlannerAssistant` into `createDefaultWorkAssistantRuntime`.
- [x] 3.3 Route `outdoor_event_detected` and `business_trip_tomorrow_detected` through `createWorkAssistantHandler` before calendar fallback.
- [x] 3.4 Preserve side-effect idempotency behavior so advisory travel responses are not cached as Lark write side effects.
- [x] 3.5 Add or update fixture events for outdoor visit, business trip, missing destination, missing origin, route failure, and weather failure cases.

## 4. Tests

- [x] 4.1 Add contract tests proving outdoor and business-trip events validate through the shared `InputEvent` envelope.
- [x] 4.2 Add handler routing tests proving travel events reach `TravelPlannerAssistant` and do not fall through to `CalendarAssistant`.
- [x] 4.3 Add travel planner unit tests for successful outdoor route reminders and recommended departure-time calculation.
- [x] 4.4 Add travel planner unit tests for missing destination, missing origin, and route adapter failure degradation.
- [x] 4.5 Add travel planner unit tests for successful business-trip weather/preparation reminders and weather adapter failure degradation.
- [x] 4.6 Add dry-run adapter tests proving deterministic route, weather, and profile behavior without network calls.
- [x] 4.7 Add scheduler-enabled dry-run smoke tests for `outdoor_event_detected` and `business_trip_tomorrow_detected` dispatch through the travel planner handler.

## 5. Documentation And Verification

- [x] 5.1 Update `plugins/work-assistant/README.md` with travel planner behavior, config examples, route/weather degradation, and scheduler rule enablement.
- [x] 5.2 Update `plugins/work-assistant/skills/work-assistant/SKILL.md` with guidance for travel reminder events and Xiaopai rendering expectations.
- [x] 5.3 Update `plugins/work-assistant/VERIFICATION.md` with travel fixture commands and expected dry-run outputs.
- [x] 5.4 Run `npm run verify` in `plugins/work-assistant`.
- [x] 5.5 Run `openspec validate add-travel-planner --strict`.
