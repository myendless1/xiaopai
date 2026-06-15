import type { TriggerPlan, TriggerRuleContext, TriggerRuleInput } from "./types.js";
export declare function generateTriggerPlans(input: TriggerRuleInput): TriggerPlan[];
export declare function createDailyBriefingPlans(context: TriggerRuleContext): TriggerPlan[];
export declare function createMeetingStartingSoonPlans(input: TriggerRuleInput): TriggerPlan[];
export declare function createOutdoorEventPlans(input: TriggerRuleInput): TriggerPlan[];
export declare function createBusinessTripTomorrowPlans(input: TriggerRuleInput): TriggerPlan[];
