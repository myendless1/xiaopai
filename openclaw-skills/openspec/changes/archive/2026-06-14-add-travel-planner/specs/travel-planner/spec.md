## ADDED Requirements

### Requirement: Outdoor event travel reminder handling
The system SHALL handle scheduler-produced `outdoor_event_detected` events as proactive same-day outdoor visit reminders.

#### Scenario: Outdoor event produces route-aware reminder
- **WHEN** `workAssistant.handleEvent` receives an `InputEvent` with `type` set to `outdoor_event_detected`, scheduler trigger metadata, and `payload.calendar_event` containing title, start, end, and a usable destination
- **THEN** the system routes the event to the travel planner assistant
- **THEN** the response includes concise speech naming the event, destination, event time, and recommended departure time when route data is available
- **THEN** the response includes a `travel.plan.generate` action with status `success`
- **THEN** the response does not create or update calendar events and does not send Lark messages

#### Scenario: Malformed outdoor event is handled without external planning calls
- **WHEN** an `outdoor_event_detected` event is missing the source calendar event summary required to generate travel guidance
- **THEN** the response includes a failed `travel.plan.generate` action
- **THEN** the system does not call route or weather adapters
- **THEN** the response does not ask the user for a follow-up caused by scheduler metadata defects

#### Scenario: Outdoor event without destination degrades safely
- **WHEN** an `outdoor_event_detected` event has a calendar summary but no usable destination can be resolved from location, description, or title
- **THEN** the response reminds the user about the outdoor event time without inventing a destination
- **THEN** the response includes a skipped `route.estimate` action or `travel.plan.generate` detail identifying missing destination data
- **THEN** no route provider call is attempted

### Requirement: Route estimate and departure-time guidance
The system SHALL estimate same-day outdoor travel timing when origin, destination, and route data are available.

#### Scenario: Successful route estimate computes departure time
- **WHEN** an outdoor travel event has a resolved destination, a configured or profiled origin address, a route mode, and a successful route estimate
- **THEN** the system computes a recommended departure time from event start time, route duration, and configured arrival buffer minutes
- **THEN** the response speech includes the route duration and recommended departure time
- **THEN** the response includes a successful `route.estimate` action with safe route details

#### Scenario: Missing origin omits precise departure time
- **WHEN** an outdoor travel event has a destination but no configured or profiled origin address
- **THEN** the response omits exact departure-time guidance
- **THEN** the response includes a skipped `route.estimate` action or `travel.plan.generate` detail identifying missing origin data
- **THEN** the response still reminds the user about the event time and destination

#### Scenario: Route adapter failure degrades gracefully
- **WHEN** the route adapter fails while estimating travel time for an outdoor event
- **THEN** the response includes a failed `route.estimate` action with a safe error code or message
- **THEN** the response still provides a bounded reminder naming the event, destination, and event time
- **THEN** the response does not throw a transport-level error

### Requirement: Business trip reminder handling
The system SHALL handle scheduler-produced `business_trip_tomorrow_detected` events as proactive next-day business trip reminders.

#### Scenario: Business trip event produces weather and preparation guidance
- **WHEN** `workAssistant.handleEvent` receives an `InputEvent` with `type` set to `business_trip_tomorrow_detected`, scheduler trigger metadata, and `payload.calendar_event` containing title, start, end, and a destination or city
- **THEN** the system routes the event to the travel planner assistant
- **THEN** the response includes concise speech naming the trip, destination, next-day time, weather summary when available, and bounded carry/preparation reminders
- **THEN** the response includes a `travel.plan.generate` action with status `success`
- **THEN** the response does not create or update calendar events and does not send Lark messages

#### Scenario: Business trip without destination degrades safely
- **WHEN** a `business_trip_tomorrow_detected` event has a calendar summary but no usable destination or city can be resolved
- **THEN** the response reminds the user about the next-day trip event without inventing a city
- **THEN** the system does not call the weather adapter
- **THEN** the response includes a skipped or failed action detail identifying missing destination data

#### Scenario: Weather adapter failure keeps preparation reminder
- **WHEN** the weather adapter fails while fetching a business trip forecast
- **THEN** the response includes a failed `weather.forecast` action with a safe error code or message
- **THEN** the response still includes bounded preparation reminders based on event title, location, and description
- **THEN** the response does not throw a transport-level error

### Requirement: Travel preparation guidance
The system SHALL generate bounded preparation guidance from calendar metadata and available weather data.

#### Scenario: Description-derived preparation note is included
- **WHEN** a travel event description contains concise preparation hints such as materials, documents, dress code, or meeting notes
- **THEN** the response includes at most one bounded preparation note derived from that description when a useful note can be identified
- **THEN** the response includes the source event id in action details for auditability

#### Scenario: Weather-derived carry note is included
- **WHEN** a business trip weather forecast indicates rain, low temperature, high temperature, or notable temperature difference
- **THEN** the response includes a concise carry note such as umbrella, jacket, or weather-appropriate clothing
- **THEN** the response avoids medical or safety claims beyond practical preparation guidance

#### Scenario: Preparation guidance remains bounded
- **WHEN** the travel planner generates speech for any travel event
- **THEN** the response speech is a single-turn robot-speakable message
- **THEN** the response includes no more than a small bounded set of preparation reminders

### Requirement: Travel adapter contracts and dry-run behavior
The system SHALL keep route, weather, and profile dependencies behind adapters and support deterministic dry-run verification.

#### Scenario: Dry-run outdoor route estimate is deterministic
- **WHEN** plugin dry-run mode is enabled and an outdoor fixture event targets a known dry-run destination
- **THEN** the dry-run route adapter returns a deterministic duration and optional distance
- **THEN** no real map provider or network call is required

#### Scenario: Dry-run business trip forecast is deterministic
- **WHEN** plugin dry-run mode is enabled and a business-trip fixture event targets a known dry-run city
- **THEN** the dry-run weather adapter returns deterministic forecast data
- **THEN** no real weather provider or network call is required

#### Scenario: Profile data supplies origin and preferences
- **WHEN** the travel planner needs origin address, route mode, or arrival buffer
- **THEN** the system reads configured or profiled travel preferences through the user-profile boundary
- **THEN** missing profile fields fall back to documented safe defaults or degraded guidance

### Requirement: Travel action reporting and context patch
The system SHALL report travel planning operations and useful short-lived context in the `StructuredResponse`.

#### Scenario: Successful outdoor reminder records travel context
- **WHEN** an outdoor travel reminder is generated successfully
- **THEN** `context_patch.current_focus` or a travel-specific context entry includes the source calendar event id, title, start time, destination, and recommended departure time when available
- **THEN** the response action details include the trigger key when available

#### Scenario: Successful business trip reminder records travel context
- **WHEN** a business trip reminder is generated successfully
- **THEN** `context_patch` includes a travel summary with source calendar event id, destination or city, trip date, and weather status when available
- **THEN** the response action details include the trigger key when available

#### Scenario: Travel reminders have no external write side effects
- **WHEN** the travel planner handles an outdoor or business-trip reminder
- **THEN** successful route, weather, profile, and travel generation actions are read-only or advisory actions
- **THEN** the system does not cache the event as a Lark write side effect unless a future change adds explicit travel write operations
