import { deriveInputEventId, deriveTriggerKey, deriveTriggerUpdateGroupKey, stableHash } from "./identity.js";
import { addLocalDays, addMinutes, formatLocalDate, getLocalDateParts, isSameLocalDate, parseLocalTime, zonedTimeToUtcIso } from "./time.js";
const DAILY_SOURCE_EVENT_ID = "daily_briefing";
export function generateTriggerPlans(input) {
    return [
        ...createDailyBriefingPlans(input),
        ...createMeetingStartingSoonPlans(input),
        ...createOutdoorEventPlans(input),
        ...createBusinessTripTomorrowPlans(input)
    ];
}
export function createDailyBriefingPlans(context) {
    const rule = context.rules.dailyBriefing;
    if (!rule.enabled)
        return [];
    const localDate = getLocalDateParts(context.now, context.timezone);
    const scheduledFor = zonedTimeToUtcIso(localDate, parseLocalTime(rule.localTime, { hour: 8, minute: 0 }), context.timezone);
    return [
        buildPlan({
            context,
            ruleId: "daily_briefing",
            type: "daily_briefing_triggered",
            scheduledFor,
            sourceEventId: `${DAILY_SOURCE_EVENT_ID}_${formatLocalDate(localDate)}`,
            eventHashInput: { rule, localDate }
        })
    ];
}
export function createMeetingStartingSoonPlans(input) {
    const rule = input.rules.meetingStartingSoon;
    if (!rule.enabled)
        return [];
    return input.events.flatMap((event) => {
        if (!isTimedEvent(event) || !matchesMeeting(event, rule.keywords))
            return [];
        return [
            buildPlan({
                context: input,
                ruleId: "meeting_starting_soon",
                type: "meeting_starting_soon",
                scheduledFor: addMinutes(event.start, -rule.offsetMinutes),
                sourceEventId: event.id,
                calendarEvent: normalizeCalendarEvent(event, input.calendarId),
                eventHashInput: { rule, event: normalizeCalendarEvent(event, input.calendarId) }
            })
        ];
    });
}
export function createOutdoorEventPlans(input) {
    const rule = input.rules.outdoorEvent;
    if (!rule.enabled)
        return [];
    return input.events.flatMap((event) => {
        if (!isTimedEvent(event) || !matchesKeywords(event, rule.keywords))
            return [];
        return [
            buildPlan({
                context: input,
                ruleId: "outdoor_event",
                type: "outdoor_event_detected",
                scheduledFor: addMinutes(event.start, -rule.offsetMinutes),
                sourceEventId: event.id,
                calendarEvent: normalizeCalendarEvent(event, input.calendarId),
                eventHashInput: { rule, event: normalizeCalendarEvent(event, input.calendarId) }
            })
        ];
    });
}
export function createBusinessTripTomorrowPlans(input) {
    const rule = input.rules.businessTripTomorrow;
    if (!rule.enabled)
        return [];
    const tomorrow = addLocalDays(getLocalDateParts(input.now, input.timezone), 1);
    const scheduledFor = zonedTimeToUtcIso(getLocalDateParts(input.now, input.timezone), parseLocalTime(rule.localTime, { hour: 18, minute: 0 }), input.timezone);
    return input.events.flatMap((event) => {
        if (!isTimedEvent(event) || !isSameLocalDate(event.start, tomorrow, input.timezone) || !matchesKeywords(event, rule.keywords)) {
            return [];
        }
        return [
            buildPlan({
                context: input,
                ruleId: "business_trip_tomorrow",
                type: "business_trip_tomorrow_detected",
                scheduledFor,
                sourceEventId: event.id,
                calendarEvent: normalizeCalendarEvent(event, input.calendarId),
                eventHashInput: { rule, event: normalizeCalendarEvent(event, input.calendarId), tomorrow }
            })
        ];
    });
}
function buildPlan(input) {
    const key = deriveTriggerKey({
        userId: input.context.userId,
        calendarId: input.context.calendarId,
        ...(input.sourceEventId ? { sourceEventId: input.sourceEventId } : {}),
        ruleId: input.ruleId,
        scheduledFor: input.scheduledFor
    });
    return {
        key,
        updateGroupKey: deriveTriggerUpdateGroupKey({
            userId: input.context.userId,
            calendarId: input.context.calendarId,
            sourceEventId: input.sourceEventId ?? input.scheduledFor,
            ruleId: input.ruleId
        }),
        eventId: deriveInputEventId(key),
        ruleId: input.ruleId,
        type: input.type,
        userId: input.context.userId,
        calendarId: input.context.calendarId,
        scheduledFor: input.scheduledFor,
        eventHash: stableHash(input.eventHashInput),
        maxAttempts: input.context.maxDispatchAttempts,
        ...(input.sourceEventId ? { sourceEventId: input.sourceEventId } : {}),
        ...(input.calendarEvent ? { calendarEvent: input.calendarEvent } : {})
    };
}
function normalizeCalendarEvent(event, fallbackCalendarId) {
    return {
        id: event.id,
        title: event.title,
        start: event.start,
        end: event.end,
        calendarId: event.calendarId ?? fallbackCalendarId,
        ...(event.location ? { location: event.location } : {}),
        ...(event.description ? { description: event.description } : {}),
        ...(event.notificationTarget ? { notificationTarget: event.notificationTarget } : {})
    };
}
function isTimedEvent(event) {
    return Number.isFinite(Date.parse(event.start)) && Number.isFinite(Date.parse(event.end));
}
function matchesMeeting(event, keywords) {
    return (event.attendeeCount ?? 0) > 1 || matchesKeywords(event, keywords) || hasLocation(event, ["会议", "meeting", "zoom", "teams"]);
}
function matchesKeywords(event, keywords) {
    const haystack = [event.title, event.location, event.description]
        .filter((value) => typeof value === "string")
        .join(" ")
        .toLocaleLowerCase();
    return keywords.some((keyword) => haystack.includes(keyword.toLocaleLowerCase()));
}
function hasLocation(event, keywords) {
    const location = event.location?.toLocaleLowerCase() ?? "";
    return keywords.some((keyword) => location.includes(keyword.toLocaleLowerCase()));
}
//# sourceMappingURL=rules.js.map