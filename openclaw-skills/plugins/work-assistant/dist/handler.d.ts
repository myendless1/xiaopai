import type { StructuredResponse } from "./contracts.js";
import type { AgendaBriefingAssistant } from "./agenda/assistant.js";
import type { CalendarAssistant } from "./calendar/assistant.js";
import type { IdempotencyStore } from "./runtime/idempotency.js";
import type { MeetingReminderAssistant } from "./meeting/assistant.js";
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
export declare function createWorkAssistantHandler(options: WorkAssistantHandlerOptions): WorkAssistantHandler;
