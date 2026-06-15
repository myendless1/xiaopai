## 1. Contract and Routing

- [x] 1.1 Reconcile this change with the active calendar structured-intent change if both touch `contracts.ts` or `handler.ts`.
- [x] 1.2 Add wellbeing event and payload types for `sedentary_detected` and `wellbeing_companion_requested`.
- [x] 1.3 Extend `WorkAssistantHandlerOptions` to accept a wellbeing companion handler.
- [x] 1.4 Route `sedentary_detected` and `wellbeing_companion_requested` to the wellbeing handler through `workAssistant.handleEvent`.
- [x] 1.5 Keep unsupported event handling and existing calendar/agenda routing behavior unchanged.

## 2. Wellbeing Domain Handler

- [x] 2.1 Create a wellbeing companion module with `WellbeingCompanionAssistant`.
- [x] 2.2 Validate sedentary payload fields including `duration_minutes` and `confidence`.
- [x] 2.3 Implement configurable thresholds for minimum sedentary duration, minimum confidence, cooldown minutes, and upcoming reminder horizon.
- [x] 2.4 Implement decision output for allowed, low-confidence, insufficient-duration, cooldown, meeting-overlap, and invalid-payload cases.
- [x] 2.5 Return stable `wellbeing.sedentary.evaluate` actions for both nudge and skipped decisions.

## 3. Calendar Context

- [x] 3.1 Reuse the existing `LarkCalendarAdapter.listEvents` method to query a bounded context window around the sedentary event timestamp.
- [x] 3.2 Detect current calendar event overlap and suppress audible sedentary nudges during meetings.
- [x] 3.3 Select at most one upcoming event within the reminder horizon for inclusion in speech and `context_patch`.
- [x] 3.4 Return degraded wellbeing responses when calendar lookup fails, including failed `lark.calendar.list` action records.
- [x] 3.5 Add dry-run calendar fixture coverage for meeting overlap and upcoming-event reminder cases.

## 4. Speech and Follow-up Content

- [x] 4.1 Implement bounded sedentary-care speech templates for movement, stretching, and eye-relaxation suggestions.
- [x] 4.2 Add presentation hints for calm nudges, skipped/quiet responses, and positive follow-up companionship responses.
- [x] 4.3 Add deterministic short joke or relaxation prompt content for `wellbeing_companion_requested`.
- [x] 4.4 Record `wellbeing.companion.generate` actions for successful follow-up content generation.
- [x] 4.5 Populate `context_patch` fields for last nudge time, last decision, follow-up offer, and nearby event summary.

## 5. Plugin Wiring and Configuration

- [x] 5.1 Instantiate `WellbeingCompanionAssistant` in the plugin factory using the existing calendar adapter.
- [x] 5.2 Add plugin config parsing for wellbeing thresholds and cooldown settings with safe defaults.
- [x] 5.3 Ensure dry-run mode supports deterministic wellbeing behavior without Lark writes.
- [x] 5.4 Export the wellbeing assistant module where useful for tests.

## 6. Tests

- [x] 6.1 Add handler routing tests for `sedentary_detected` and `wellbeing_companion_requested`.
- [x] 6.2 Add wellbeing assistant tests for valid nudge generation and invalid payload skips.
- [x] 6.3 Add threshold tests for low confidence and short duration.
- [x] 6.4 Add cooldown suppression tests using returned or supplied context.
- [x] 6.5 Add calendar-aware tests for meeting overlap, upcoming event reminder, and calendar lookup failure.
- [x] 6.6 Add follow-up companionship tests for bounded content and action records.
- [x] 6.7 Add contract tests for updated action records and structured response validation.

## 7. Documentation and Fixtures

- [x] 7.1 Add sample `sedentary_detected` and `wellbeing_companion_requested` fixture events.
- [x] 7.2 Update `plugins/work-assistant/README.md` with wellbeing event examples and response behavior.
- [x] 7.3 Update `plugins/work-assistant/skills/work-assistant/SKILL.md` with fixed-format robot message normalization guidance.
- [x] 7.4 Document that robot sensing, camera capture, and hardware control remain outside the plugin.

## 8. Verification

- [x] 8.1 Run plugin unit and integration tests with `npm run verify` from `plugins/work-assistant`.
- [x] 8.2 Run strict OpenSpec validation for `add-wellbeing-companion`.
- [x] 8.3 Build and reinstall or reload the local plugin in dry-run mode.
- [x] 8.4 Verify dry-run Gateway calls for sedentary nudge, skipped reminder, and follow-up companionship fixtures.
- [x] 8.5 Record verification commands and results in `plugins/work-assistant/VERIFICATION.md`.
