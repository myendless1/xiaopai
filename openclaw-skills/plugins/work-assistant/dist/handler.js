import { followUpResponse, unsupportedEventResponse, validateInputEvent } from "./contracts.js";
import { MemoryIdempotencyStore } from "./runtime/idempotency.js";
import { shouldRouteToMeetingNotification } from "./meeting/assistant.js";
export function createWorkAssistantHandler(options) {
    const idempotencyStore = options.idempotencyStore ?? new MemoryIdempotencyStore();
    return {
        async handleEvent(rawEvent) {
            const validated = validateInputEvent(rawEvent);
            if (!validated.ok)
                return followUpResponse(validated.message, "invalid_input", "事件格式不完整，请检查调用参数。");
            const event = validated.value;
            if (!isSupportedEventType(event.type))
                return unsupportedEventResponse(event.type);
            if (isAgendaBriefingEventType(event.type)) {
                return options.agendaBriefingAssistant.handle(event);
            }
            if (isWellbeingEventType(event.type)) {
                return options.wellbeingCompanionAssistant.handle(event);
            }
            if (isTravelEventType(event.type)) {
                return dispatchTravelEvent(event, options.travelPlannerAssistant);
            }
            const cached = await idempotencyStore.get(event.event_id);
            if (cached)
                return cached;
            const response = await dispatchEvent(event, options.calendarAssistant, options.meetingReminderAssistant);
            if (hasSideEffect(response))
                await idempotencyStore.set(event.event_id, response);
            return response;
        }
    };
}
async function dispatchTravelEvent(event, travelPlannerAssistant) {
    if (event.type === "outdoor_event_detected")
        return travelPlannerAssistant.handleOutdoorEvent(event);
    return travelPlannerAssistant.handleBusinessTripTomorrow(event);
}
async function dispatchEvent(event, calendarAssistant, meetingReminderAssistant) {
    if (event.type === "meeting_starting_soon") {
        return meetingReminderAssistant.handleReminder(event);
    }
    if (shouldRouteToMeetingNotification(event)) {
        return meetingReminderAssistant.handleLateNotification(event);
    }
    return calendarAssistant.handle(event);
}
function isSupportedEventType(type) {
    return (type === "user_utterance" ||
        type === "meeting_starting_soon" ||
        isAgendaBriefingEventType(type) ||
        isTravelEventType(type) ||
        isWellbeingEventType(type));
}
function isAgendaBriefingEventType(type) {
    return type === "head_touch" || type === "daily_briefing_triggered";
}
function isWellbeingEventType(type) {
    return type === "sedentary_detected" || type === "wellbeing_companion_requested";
}
function isTravelEventType(type) {
    return type === "outdoor_event_detected" || type === "business_trip_tomorrow_detected";
}
function hasSideEffect(response) {
    return response.actions.some((action) => (action.type === "lark.calendar.create" || action.type === "lark.message.send") &&
        action.status === "success");
}
//# sourceMappingURL=handler.js.map