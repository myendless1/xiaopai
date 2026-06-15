import type { InputEvent, StructuredResponse } from "../contracts.js";
import type { AgendaEventCategory, LarkCalendarAdapter, NormalizedAgendaEvent } from "../lark/adapters.js";
type AgendaBriefingAssistantOptions = {
    calendarAdapter: LarkCalendarAdapter;
    calendarId?: string;
    maxHighlights?: number;
    rules?: ClassificationRules;
};
export type ClassificationRules = Record<AgendaEventCategory, string[]>;
export type ClassifiedAgendaEvent = NormalizedAgendaEvent & {
    category: AgendaEventCategory;
    classificationReason: string;
};
export type BriefingWindow = {
    start: string;
    end: string;
    localDate: string;
};
export declare class AgendaBriefingAssistant {
    private readonly calendarAdapter;
    private readonly calendarId;
    private readonly maxHighlights;
    private readonly rules;
    constructor(options: AgendaBriefingAssistantOptions);
    handle(event: InputEvent): Promise<StructuredResponse>;
    private listWindow;
}
export declare function calculateTodayWindow(timestamp: string, timezone: string): BriefingWindow;
export declare function calculatePreviousWorkWeekWindow(timestamp: string, timezone: string): BriefingWindow;
export declare function classifyAgendaEvent(agendaEvent: NormalizedAgendaEvent, rules?: ClassificationRules): ClassifiedAgendaEvent;
export declare function countByCategory(events: ClassifiedAgendaEvent[]): Record<AgendaEventCategory, number>;
export declare function selectAgendaHighlights(events: ClassifiedAgendaEvent[], limit?: number): ClassifiedAgendaEvent[];
export {};
