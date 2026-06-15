import { ProactiveCalendarTriggerScheduler, type SchedulerAgentDispatchRuntimeApi, type ProactiveCalendarSchedulerConfig } from "./scheduler/index.js";
import type { TravelPlannerConfig } from "./travel/adapters.js";
import { type WellbeingCompanionConfig } from "./wellbeing/assistant.js";
type PluginConfig = {
    dryRun?: boolean;
    larkCliPath?: string;
    larkIdentity?: "user" | "bot";
    timeoutMs?: number;
    travel?: TravelPlannerConfig;
    wellbeing?: WellbeingCompanionConfig;
    scheduler: ProactiveCalendarSchedulerConfig;
};
type RuntimeApi = SchedulerAgentDispatchRuntimeApi & {
    pluginConfig?: unknown;
};
export declare function createDefaultWorkAssistant(api: {
    pluginConfig?: unknown;
}): import("./handler.js").WorkAssistantHandler;
export declare function createDefaultWorkAssistantRuntime(api: RuntimeApi): {
    assistant: import("./handler.js").WorkAssistantHandler;
    scheduler: ProactiveCalendarTriggerScheduler | undefined;
    config: PluginConfig;
};
export declare const workAssistantPlugin: {
    id: string;
    name: string;
    description: string;
    configSchema: import("openclaw/plugin-sdk/plugin-entry").OpenClawPluginConfigSchema;
    register: NonNullable<import("openclaw/plugin-sdk/plugin-entry").OpenClawPluginDefinition["register"]>;
} & Pick<import("openclaw/plugin-sdk/plugin-entry").OpenClawPluginDefinition, "kind" | "reload" | "nodeHostCommands" | "securityAuditCollectors">;
export default workAssistantPlugin;
export { AgendaBriefingAssistant } from "./agenda/assistant.js";
export { CalendarAssistant } from "./calendar/assistant.js";
export { CalendarIntentParser } from "./calendar/parser.js";
export { createWorkAssistantHandler } from "./handler.js";
export { MeetingReminderAssistant } from "./meeting/assistant.js";
export { MemoryIdempotencyStore } from "./runtime/idempotency.js";
export { JsonFileTriggerPlanStore, MemoryTriggerPlanStore, ProactiveCalendarTriggerScheduler, readSchedulerConfig } from "./scheduler/index.js";
export { TravelPlannerAssistant } from "./travel/assistant.js";
export { ConfiguredUserProfileAdapter, createDryRunUserProfileAdapter, DryRunRouteAdapter, DryRunWeatherAdapter, UnavailableRouteAdapter, UnavailableWeatherAdapter } from "./travel/dry-run.js";
export { QWeatherWeatherAdapter } from "./travel/qweather.js";
export { WellbeingCompanionAssistant } from "./wellbeing/assistant.js";
