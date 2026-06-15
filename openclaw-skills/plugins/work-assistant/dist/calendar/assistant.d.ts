import type { InputEvent, StructuredResponse } from "../contracts.js";
import type { LarkCalendarAdapter, LarkContactAdapter } from "../lark/adapters.js";
import { CalendarIntentParser } from "./parser.js";
export type CalendarAssistantOptions = {
    contactAdapter: LarkContactAdapter;
    calendarAdapter: LarkCalendarAdapter;
    parser?: CalendarIntentParser;
};
export declare class CalendarAssistant {
    private readonly contactAdapter;
    private readonly calendarAdapter;
    private readonly parser;
    constructor(options: CalendarAssistantOptions);
    handle(event: InputEvent): Promise<StructuredResponse>;
}
