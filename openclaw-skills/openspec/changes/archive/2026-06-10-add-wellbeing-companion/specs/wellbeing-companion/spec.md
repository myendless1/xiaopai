## ADDED Requirements

### Requirement: Sedentary detection event handling
The system SHALL handle sedentary detection events from a robot or sensing service using the existing `InputEvent` and `StructuredResponse` envelope.

#### Scenario: Valid sedentary event is accepted
- **WHEN** `workAssistant.handleEvent` receives an `InputEvent` with `type` set to `sedentary_detected`, valid `timestamp`, valid `context.timezone`, numeric `payload.duration_minutes`, and numeric `payload.confidence`
- **THEN** the system routes the event to the wellbeing companion handler
- **THEN** the system returns a `StructuredResponse` with `speech`, `presentation`, `actions`, `follow_up`, and `context_patch`

#### Scenario: Invalid sedentary payload is rejected without a nudge
- **WHEN** a `sedentary_detected` event is missing `payload.duration_minutes` or `payload.confidence`
- **THEN** the system returns a structured response that records a skipped `wellbeing.sedentary.evaluate` action
- **THEN** the system does not generate sedentary-care nudge speech

### Requirement: Sedentary interruption decision
The wellbeing companion SHALL decide whether to interrupt the user before generating audible sedentary-care speech.

#### Scenario: Low-confidence sedentary event is skipped
- **WHEN** a `sedentary_detected` event has confidence below the configured sedentary confidence threshold
- **THEN** the response includes a skipped `wellbeing.sedentary.evaluate` action with a low-confidence reason
- **THEN** `follow_up.expected` is false
- **THEN** the system does not generate audible wellbeing nudge speech

#### Scenario: Short sedentary duration is skipped
- **WHEN** a `sedentary_detected` event has duration below the configured sedentary duration threshold
- **THEN** the response includes a skipped `wellbeing.sedentary.evaluate` action with an insufficient-duration reason
- **THEN** the system does not generate audible wellbeing nudge speech

#### Scenario: Recent wellbeing nudge suppresses duplicate reminder
- **WHEN** a `sedentary_detected` event includes context showing that the user was recently nudged inside the configured cooldown window
- **THEN** the response includes a skipped `wellbeing.sedentary.evaluate` action with a cooldown reason
- **THEN** the response does not ask the user for a follow-up companionship turn

### Requirement: Calendar-aware interruption context
The wellbeing companion SHALL use available calendar context to avoid poorly timed interruptions and to mention nearby events when useful.

#### Scenario: Meeting overlap suppresses sedentary nudge
- **WHEN** the calendar adapter returns a current event that overlaps the sedentary event timestamp
- **THEN** the response includes a successful `lark.calendar.list` action for the queried context window
- **THEN** the response includes a skipped `wellbeing.sedentary.evaluate` action with a meeting-overlap reason
- **THEN** the system does not generate audible sedentary-care nudge speech

#### Scenario: Upcoming event can be included in nudge
- **WHEN** a sedentary nudge is allowed and the calendar adapter returns an upcoming event within the configured reminder horizon
- **THEN** the response speech includes at most one concise upcoming-event reminder
- **THEN** `context_patch` includes a `wellbeing_nearby_event` summary

#### Scenario: Calendar lookup failure degrades gracefully
- **WHEN** the calendar adapter fails while checking wellbeing context
- **THEN** the response includes a failed `lark.calendar.list` action with an error code or message
- **THEN** the system continues the sedentary interruption decision using payload and recent-context data only
- **THEN** the system does not perform any Lark write action

### Requirement: Wellbeing nudge response generation
The wellbeing companion SHALL generate bounded, robot-speakable wellbeing responses when a sedentary nudge is allowed.

#### Scenario: Allowed sedentary nudge includes care guidance
- **WHEN** a `sedentary_detected` event passes validation and interruption checks
- **THEN** the response speech recommends a short movement, stretching, or eye-relaxation break without making medical claims
- **THEN** the response presentation hints use a calm or gentle expression, motion, and light state
- **THEN** the response includes a successful `wellbeing.sedentary.evaluate` action

#### Scenario: Allowed sedentary nudge offers light companionship
- **WHEN** a sedentary nudge is generated
- **THEN** `follow_up.expected` is true
- **THEN** `follow_up.question` asks whether the user wants a short joke, relaxation prompt, or similar light companionship
- **THEN** `context_patch` records `wellbeing_last_nudge_at` and `wellbeing_follow_up_offered`

### Requirement: Wellbeing follow-up companionship
The system SHALL support a follow-up wellbeing turn when the user accepts light companionship after a sedentary nudge.

#### Scenario: User accepts companionship offer
- **WHEN** `workAssistant.handleEvent` receives an `InputEvent` with `type` set to `wellbeing_companion_requested`
- **THEN** the system routes the event to the wellbeing companion handler
- **THEN** the response speech contains one short joke, relaxation prompt, or light companionship message
- **THEN** the response presentation hints use a positive expression, motion, or light state

#### Scenario: Follow-up content remains bounded
- **WHEN** the wellbeing companion generates follow-up content
- **THEN** the response speech contains a bounded single-turn message suitable for robot voice playback
- **THEN** `follow_up.expected` is false unless the payload explicitly asks for another wellbeing turn
- **THEN** the response records a successful `wellbeing.companion.generate` action
