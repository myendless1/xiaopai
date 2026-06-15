import type { InputEvent } from "../contracts.js";
export type CalendarCreateIntent = {
    title?: string;
    attendeeNames: string[];
    start?: string;
    end?: string;
    ambiguity?: string;
    confidence: number;
};
export declare class CalendarIntentParser {
    parse(event: InputEvent): CalendarCreateIntent;
    private extractTitle;
    private extractAttendees;
    private extractTimes;
}
