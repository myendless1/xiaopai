## 1. Contract and Validation

- [x] 1.1 Add TypeScript types for `StructuredAssistantIntent`, `StructuredCalendarCreateIntent`, and structured attendee references.
- [x] 1.2 Add runtime validation helpers for `payload.structured_intent`, including intent type, version, title, ISO start/end, and attendee references.
- [x] 1.3 Add validation result reasons for malformed structured intent, unsupported intent type, unsupported version, missing required fields, invalid time range, and invalid attendee reference.
- [x] 1.4 Preserve existing `InputEvent` and `StructuredResponse` external shapes while documenting the optional `payload.structured_intent` field.

## 2. Calendar Assistant Execution

- [x] 2.1 Refactor calendar input normalization so `CalendarAssistant` can consume either structured intent or parser output.
- [x] 2.2 Prefer validated `payload.structured_intent.type = "calendar.create"` over natural-language parsing when present.
- [x] 2.3 Keep `CalendarIntentParser` as a fallback only when structured intent is absent.
- [x] 2.4 Support structured attendees by name through the existing `LarkContactAdapter`.
- [x] 2.5 Support valid Lark attendee IDs (`ou_`, `oc_`, `omm_`) without contact lookup.
- [x] 2.6 Ensure malformed or partial structured intent returns a follow-up response and does not call Lark adapters.

## 3. Tests

- [x] 3.1 Add parser-bypass tests proving flexible text works when structured intent is present.
- [x] 3.2 Add success tests for structured intent with attendee name resolution.
- [x] 3.3 Add success tests for structured intent with direct attendee IDs.
- [x] 3.4 Add failure tests for malformed structured intent, unsupported type, unsupported version, missing fields, invalid time ranges, invalid attendees, ambiguous attendees, and missing attendees.
- [x] 3.5 Add idempotency tests for duplicate structured-intent events.
- [x] 3.6 Keep existing text-only fixture and parser tests passing.

## 4. Documentation and Skill Guidance

- [x] 4.1 Update `README.md` with the preferred structured-intent request shape and the legacy text fallback.
- [x] 4.2 Update `skills/work-assistant/SKILL.md` to instruct OpenClaw to extract a `calendar.create` structured intent before calling `workAssistant.handleEvent`.
- [x] 4.3 Add examples for flexible wording such as "明天上午10点到10点半有个活动 OpenClaw 测试，帮我建一个飞书日程，邀请 Gargantua 参会".
- [x] 4.4 Document that the plugin validates and executes but does not perform model-backed natural-language inference.

## 5. Verification

- [x] 5.1 Run `npm run verify` in `plugins/work-assistant`.
- [x] 5.2 Run `openspec validate split-calendar-intent-extraction-execution --strict`.
- [x] 5.3 Verify dry-run Gateway invocation with a structured intent and flexible original text.
- [ ] 5.4 Verify real Lark write only after explicit operator confirmation, using a unique `event_id`.
- [x] 5.5 Verify duplicate submission of the same structured-intent `event_id` returns an equivalent response without creating a second calendar event.
