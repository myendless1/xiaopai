import type { InputEvent, StructuredResponse } from "../contracts.js";
import type { LarkCalendarAdapter } from "../lark/adapters.js";
export type WellbeingDecision = "allowed" | "low_confidence" | "insufficient_duration" | "cooldown" | "meeting_overlap" | "invalid_payload";
export type WellbeingCompanionConfig = {
    minimumSedentaryDurationMinutes?: number;
    minimumConfidence?: number;
    cooldownMinutes?: number;
    upcomingReminderHorizonMinutes?: number;
};
export type WellbeingCompanionAssistantOptions = WellbeingCompanionConfig & {
    calendarAdapter: LarkCalendarAdapter;
    calendarId?: string;
};
export declare class WellbeingCompanionAssistant {
    private readonly calendarAdapter;
    private readonly calendarId;
    private readonly minimumSedentaryDurationMinutes;
    private readonly minimumConfidence;
    private readonly cooldownMinutes;
    private readonly upcomingReminderHorizonMinutes;
    constructor(options: WellbeingCompanionAssistantOptions);
    handle(event: InputEvent): Promise<StructuredResponse>;
    private handleSedentaryDetected;
    private handleCompanionRequest;
    private listCalendarContext;
    private evaluateCooldown;
    private skippedResponse;
    private evaluateAction;
}
