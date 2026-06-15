import type { InputEvent, StructuredResponse } from "./contracts.js";
import { followUpResponse, unsupportedEventResponse, validateInputEvent } from "./contracts.js";
import type { AgendaBriefingAssistant } from "./agenda/assistant.js";
import type { CalendarAssistant } from "./calendar/assistant.js";
import type { IdempotencyStore } from "./runtime/idempotency.js";
import { MemoryIdempotencyStore } from "./runtime/idempotency.js";
import type { MeetingReminderAssistant } from "./meeting/assistant.js";
import { shouldRouteToMeetingNotification } from "./meeting/assistant.js";
import type { TravelPlannerAssistant } from "./travel/assistant.js";
import type { WellbeingCompanionAssistant } from "./wellbeing/assistant.js";

export type WorkAssistantHandlerOptions = {
  calendarAssistant: CalendarAssistant;
  agendaBriefingAssistant: AgendaBriefingAssistant;
  meetingReminderAssistant: MeetingReminderAssistant;
  travelPlannerAssistant: TravelPlannerAssistant;
  wellbeingCompanionAssistant: WellbeingCompanionAssistant;
  idempotencyStore?: IdempotencyStore;
};

export type WorkAssistantHandler = {
  handleEvent(event: unknown): Promise<StructuredResponse>;
};

export function createWorkAssistantHandler(options: WorkAssistantHandlerOptions): WorkAssistantHandler {
  const idempotencyStore = options.idempotencyStore ?? new MemoryIdempotencyStore();

  return {
    async handleEvent(rawEvent: unknown): Promise<StructuredResponse> {
      const validated = validateInputEvent(rawEvent);
      if (!validated.ok) return followUpResponse(validated.message, "invalid_input", "事件格式不完整，请检查调用参数。");

      const event = validated.value;
      if (!isSupportedEventType(event.type)) return unsupportedEventResponse(event.type);

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
      if (cached) return cached;

      const response = await dispatchEvent(event, options.calendarAssistant, options.meetingReminderAssistant);
      if (hasSideEffect(response)) await idempotencyStore.set(event.event_id, response);
      return response;
    }
  };
}

async function dispatchTravelEvent(
  event: InputEvent,
  travelPlannerAssistant: TravelPlannerAssistant
): Promise<StructuredResponse> {
  if (event.type === "outdoor_event_detected") return travelPlannerAssistant.handleOutdoorEvent(event);
  return travelPlannerAssistant.handleBusinessTripTomorrow(event);
}

async function dispatchEvent(
  event: InputEvent,
  calendarAssistant: CalendarAssistant,
  meetingReminderAssistant: MeetingReminderAssistant
): Promise<StructuredResponse> {
  if (event.type === "meeting_starting_soon") {
    return meetingReminderAssistant.handleReminder(event);
  }
  if (shouldRouteToMeetingNotification(event)) {
    return meetingReminderAssistant.handleLateNotification(event);
  }
  return calendarAssistant.handle(event);
}

function isSupportedEventType(type: string): boolean {
  return (
    type === "user_utterance" ||
    type === "meeting_starting_soon" ||
    isAgendaBriefingEventType(type) ||
    isTravelEventType(type) ||
    isWellbeingEventType(type)
  );
}

function isAgendaBriefingEventType(type: string): boolean {
  return type === "head_touch" || type === "daily_briefing_triggered";
}

function isWellbeingEventType(type: string): boolean {
  return type === "sedentary_detected" || type === "wellbeing_companion_requested";
}

function isTravelEventType(type: string): boolean {
  return type === "outdoor_event_detected" || type === "business_trip_tomorrow_detected";
}

function hasSideEffect(response: StructuredResponse): boolean {
  return response.actions.some(
    (action) =>
      (action.type === "lark.calendar.create" || action.type === "lark.message.send") &&
      action.status === "success"
  );
}
