function isRecord(value) {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}
function isValidIsoTimestamp(value) {
    const parsed = Date.parse(value);
    return Number.isFinite(parsed);
}
function hasValidLarkAttendeeIdPrefix(value) {
    return /^(ou_|oc_|omm_).+/.test(value);
}
export function validateInputEvent(value) {
    if (!isRecord(value))
        return { ok: false, message: "InputEvent must be an object." };
    const eventId = value.event_id;
    const type = value.type;
    const timestamp = value.timestamp;
    const userId = value.user_id;
    const payload = value.payload;
    const context = value.context;
    if (typeof eventId !== "string" || eventId.trim() === "") {
        return { ok: false, message: "InputEvent.event_id must be a non-empty string." };
    }
    if (typeof type !== "string" || type.trim() === "") {
        return { ok: false, message: "InputEvent.type must be a non-empty string." };
    }
    if (typeof timestamp !== "string" || !isValidIsoTimestamp(timestamp)) {
        return { ok: false, message: "InputEvent.timestamp must be an ISO timestamp." };
    }
    if (typeof userId !== "string" || userId.trim() === "") {
        return { ok: false, message: "InputEvent.user_id must be a non-empty string." };
    }
    if (!isRecord(payload)) {
        return { ok: false, message: "InputEvent.payload must be an object." };
    }
    if (!isRecord(context) || typeof context.timezone !== "string" || context.timezone.trim() === "") {
        return { ok: false, message: "InputEvent.context.timezone must be a non-empty string." };
    }
    return {
        ok: true,
        value: {
            event_id: eventId,
            type,
            timestamp,
            user_id: userId,
            payload,
            context: { ...context, timezone: context.timezone }
        }
    };
}
export function validateStructuredAssistantIntent(value) {
    if (!isRecord(value)) {
        return {
            ok: false,
            reason: "malformed_structured_intent",
            message: "Structured intent must be an object."
        };
    }
    if (typeof value.type !== "string" || value.type.trim() === "") {
        return {
            ok: false,
            reason: "malformed_structured_intent",
            message: "Structured intent type must be a non-empty string."
        };
    }
    if (value.type !== "calendar.create") {
        if (value.type === "meeting.notify_late")
            return validateMeetingNotifyLateIntent(value);
        return {
            ok: false,
            reason: "unsupported_intent_type",
            message: "Only calendar.create and meeting.notify_late structured intents are supported."
        };
    }
    if (typeof value.version !== "string" || value.version.trim() === "") {
        return {
            ok: false,
            reason: "missing_required_structured_fields",
            message: "Structured calendar intent version is required."
        };
    }
    if (value.version !== "1") {
        return {
            ok: false,
            reason: "unsupported_intent_version",
            message: "Structured calendar intent version is not supported."
        };
    }
    if (typeof value.title !== "string" || value.title.trim() === "") {
        return {
            ok: false,
            reason: "missing_required_structured_fields",
            message: "Structured calendar intent title is required."
        };
    }
    if (typeof value.start !== "string" || !isValidIsoTimestamp(value.start)) {
        return {
            ok: false,
            reason: "missing_required_structured_fields",
            message: "Structured calendar intent start must be an ISO timestamp."
        };
    }
    if (typeof value.end !== "string" || !isValidIsoTimestamp(value.end)) {
        return {
            ok: false,
            reason: "missing_required_structured_fields",
            message: "Structured calendar intent end must be an ISO timestamp."
        };
    }
    if (Date.parse(value.end) <= Date.parse(value.start)) {
        return {
            ok: false,
            reason: "invalid_time_range",
            message: "Structured calendar intent end must be after start."
        };
    }
    if (!Array.isArray(value.attendees) || value.attendees.length === 0) {
        return {
            ok: false,
            reason: "missing_required_structured_fields",
            message: "Structured calendar intent requires at least one attendee."
        };
    }
    const attendees = [];
    for (const attendee of value.attendees) {
        if (!isRecord(attendee)) {
            return {
                ok: false,
                reason: "invalid_attendee_reference",
                message: "Structured attendee references must be objects."
            };
        }
        const name = attendee.name;
        const id = attendee.id;
        if (typeof id === "string" && id.trim() !== "") {
            const normalizedId = id.trim();
            if (!hasValidLarkAttendeeIdPrefix(normalizedId)) {
                return {
                    ok: false,
                    reason: "invalid_attendee_reference",
                    message: "Structured attendee id must use a supported Lark attendee prefix."
                };
            }
            attendees.push({ id: normalizedId });
            continue;
        }
        if (typeof name === "string" && name.trim() !== "") {
            attendees.push({ name: name.trim() });
            continue;
        }
        return {
            ok: false,
            reason: "invalid_attendee_reference",
            message: "Structured attendee must include a non-empty name or valid Lark attendee id."
        };
    }
    return {
        ok: true,
        value: {
            type: "calendar.create",
            version: "1",
            title: value.title.trim(),
            start: value.start,
            end: value.end,
            attendees
        }
    };
}
function validateMeetingNotifyLateIntent(value) {
    if (typeof value.version !== "string" || value.version.trim() === "") {
        return {
            ok: false,
            reason: "missing_required_structured_fields",
            message: "Structured meeting notification intent version is required."
        };
    }
    if (value.version !== "1") {
        return {
            ok: false,
            reason: "unsupported_intent_version",
            message: "Structured meeting notification intent version is not supported."
        };
    }
    const intent = {
        type: "meeting.notify_late",
        version: "1"
    };
    if (value.delay_minutes !== undefined) {
        if (typeof value.delay_minutes !== "number" ||
            !Number.isInteger(value.delay_minutes) ||
            value.delay_minutes < 1 ||
            value.delay_minutes > 180) {
            return {
                ok: false,
                reason: "missing_required_structured_fields",
                message: "Structured meeting notification delay_minutes must be an integer from 1 to 180."
            };
        }
        intent.delay_minutes = value.delay_minutes;
    }
    if (value.message !== undefined) {
        if (typeof value.message !== "string" || value.message.trim() === "") {
            return {
                ok: false,
                reason: "missing_required_structured_fields",
                message: "Structured meeting notification message must be a non-empty string when provided."
            };
        }
        intent.message = value.message.trim().slice(0, 200);
    }
    return {
        ok: true,
        value: intent
    };
}
export function validateStructuredResponse(value) {
    if (!isRecord(value))
        return { ok: false, message: "StructuredResponse must be an object." };
    if (typeof value.speech !== "string")
        return { ok: false, message: "speech must be a string." };
    if (!isRecord(value.presentation))
        return { ok: false, message: "presentation must be an object." };
    if (!Array.isArray(value.actions))
        return { ok: false, message: "actions must be an array." };
    if (!isRecord(value.follow_up) || typeof value.follow_up.expected !== "boolean") {
        return { ok: false, message: "follow_up.expected must be a boolean." };
    }
    if (!isRecord(value.context_patch))
        return { ok: false, message: "context_patch must be an object." };
    return { ok: true, value: value };
}
export function followUpResponse(question, reason, speech = question) {
    return {
        speech,
        presentation: {
            emotion: "thinking",
            motion: "look_at_user",
            light: "soft_blink"
        },
        actions: [],
        follow_up: {
            expected: true,
            question,
            reason
        },
        context_patch: {}
    };
}
export function unsupportedEventResponse(type) {
    return {
        speech: `暂时还不支持处理 ${type} 类型的事件。`,
        presentation: {
            emotion: "neutral",
            motion: "none",
            light: "none"
        },
        actions: [],
        follow_up: {
            expected: false
        },
        context_patch: {}
    };
}
//# sourceMappingURL=contracts.js.map