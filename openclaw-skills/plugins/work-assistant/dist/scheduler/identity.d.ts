import type { ProactiveCalendarRuleId } from "./types.js";
export declare function stableJson(value: unknown): string;
export declare function stableHash(value: unknown): string;
export declare function deriveTriggerKey(input: {
    userId: string;
    calendarId: string;
    sourceEventId?: string;
    ruleId: ProactiveCalendarRuleId;
    scheduledFor: string;
}): string;
export declare function deriveTriggerUpdateGroupKey(input: {
    userId: string;
    calendarId: string;
    sourceEventId: string;
    ruleId: ProactiveCalendarRuleId;
}): string;
export declare function deriveInputEventId(triggerKey: string): string;
