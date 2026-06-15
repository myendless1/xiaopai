import { definePluginEntry } from "openclaw/plugin-sdk/plugin-entry";
import { AgendaBriefingAssistant } from "./agenda/assistant.js";
import { CalendarAssistant } from "./calendar/assistant.js";
import { createWorkAssistantHandler } from "./handler.js";
import { DryRunCalendarAdapter, DryRunContactAdapter, DryRunIMAdapter } from "./lark/dry-run.js";
import { LarkCliCalendarAdapter, LarkCliContactAdapter, LarkCliIMAdapter } from "./lark/lark-cli.js";
import { MeetingReminderAssistant } from "./meeting/assistant.js";
import { dispatchSchedulerResponseToAgent, JsonFileTriggerPlanStore, ProactiveCalendarTriggerScheduler, readSchedulerConfig } from "./scheduler/index.js";
import { TravelPlannerAssistant } from "./travel/assistant.js";
import { ConfiguredUserProfileAdapter, createDryRunUserProfileAdapter, DryRunRouteAdapter, DryRunWeatherAdapter, UnavailableRouteAdapter, UnavailableWeatherAdapter } from "./travel/dry-run.js";
import { QWeatherWeatherAdapter } from "./travel/qweather.js";
import { WellbeingCompanionAssistant } from "./wellbeing/assistant.js";
function readPluginConfig(api) {
    const raw = typeof api.pluginConfig === "object" && api.pluginConfig !== null ? api.pluginConfig : {};
    const config = {
        dryRun: raw.dryRun === true,
        larkIdentity: raw.larkIdentity === "bot" ? "bot" : "user",
        scheduler: readSchedulerConfig(raw.scheduler)
    };
    if (typeof raw.larkCliPath === "string")
        config.larkCliPath = raw.larkCliPath;
    if (typeof raw.timeoutMs === "number")
        config.timeoutMs = raw.timeoutMs;
    const travel = readTravelConfig(raw.travel);
    if (travel)
        config.travel = travel;
    const wellbeing = readWellbeingConfig(raw.wellbeing);
    if (wellbeing)
        config.wellbeing = wellbeing;
    return config;
}
export function createDefaultWorkAssistant(api) {
    return createDefaultWorkAssistantRuntime(api).assistant;
}
export function createDefaultWorkAssistantRuntime(api) {
    const config = readPluginConfig(api);
    const cliOptions = {
        ...(config.larkCliPath ? { cliPath: config.larkCliPath } : {}),
        ...(config.larkIdentity ? { identity: config.larkIdentity } : {}),
        ...(config.timeoutMs ? { timeoutMs: config.timeoutMs } : {})
    };
    const contactAdapter = config.dryRun
        ? new DryRunContactAdapter()
        : new LarkCliContactAdapter(cliOptions);
    const calendarAdapter = config.dryRun
        ? new DryRunCalendarAdapter()
        : new LarkCliCalendarAdapter(cliOptions);
    const imAdapter = config.dryRun
        ? new DryRunIMAdapter()
        : new LarkCliIMAdapter(cliOptions);
    const travelConfig = config.travel ?? {};
    const routeAdapter = config.dryRun ? new DryRunRouteAdapter() : new UnavailableRouteAdapter();
    const weatherAdapter = config.dryRun
        ? new DryRunWeatherAdapter()
        : travelConfig.weather?.provider === "qweather"
            ? new QWeatherWeatherAdapter({ config: travelConfig.weather })
            : new UnavailableWeatherAdapter();
    const userProfileAdapter = config.dryRun
        ? createDryRunUserProfileAdapter(travelConfig)
        : new ConfiguredUserProfileAdapter(travelConfig);
    const assistant = createWorkAssistantHandler({
        calendarAssistant: new CalendarAssistant({
            contactAdapter,
            calendarAdapter
        }),
        agendaBriefingAssistant: new AgendaBriefingAssistant({
            calendarAdapter
        }),
        meetingReminderAssistant: new MeetingReminderAssistant({
            imAdapter
        }),
        travelPlannerAssistant: new TravelPlannerAssistant({
            routeAdapter,
            weatherAdapter,
            userProfileAdapter,
            ...travelConfig
        }),
        wellbeingCompanionAssistant: new WellbeingCompanionAssistant({
            calendarAdapter,
            ...(config.wellbeing ?? {})
        })
    });
    const scheduler = config.scheduler.enabled
        ? new ProactiveCalendarTriggerScheduler({
            config: config.scheduler,
            calendarAdapter,
            store: config.scheduler.statePath
                ? new JsonFileTriggerPlanStore(config.scheduler.statePath)
                : new JsonFileTriggerPlanStore(`${process.cwd()}/.openclaw/work-assistant-scheduler-state.json`),
            dispatch: async (event) => {
                const response = await assistant.handleEvent(event);
                const agentDispatch = await dispatchSchedulerResponseToAgent({
                    api,
                    event,
                    response,
                    config: config.scheduler.agentDispatch
                });
                if (agentDispatch.status === "failed") {
                    api.logger?.warn?.(`work-assistant scheduler agent dispatch failed ${JSON.stringify({
                        event_id: event.event_id,
                        code: agentDispatch.code,
                        message: agentDispatch.message
                    })}`);
                    return {
                        ok: false,
                        code: agentDispatch.code,
                        message: agentDispatch.message
                    };
                }
                return {
                    ok: true,
                    response
                };
            }
        })
        : undefined;
    return {
        assistant,
        scheduler,
        config
    };
}
export const workAssistantPlugin = definePluginEntry({
    id: "work-assistant",
    name: "Work Assistant",
    description: "Workplace assistant event handling and Lark calendar creation.",
    register(api) {
        const runtime = createDefaultWorkAssistantRuntime(api);
        const assistant = runtime.assistant;
        let loop;
        api.registerGatewayMethod("workAssistant.handleEvent", async ({ params, respond }) => {
            const event = "event" in params ? params.event : params;
            const response = await assistant.handleEvent(event);
            respond(true, response);
        }, { scope: "operator.write" });
        if (runtime.scheduler && runtime.config.scheduler.startIntervalLoop) {
            loop = startSchedulerLoop(runtime.scheduler, runtime.config.scheduler);
            registerDisposal(api, () => loop?.stop());
        }
    }
});
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
function readTravelConfig(value) {
    const raw = typeof value === "object" && value !== null && !Array.isArray(value) ? value : {};
    const config = {};
    const originAddress = readNonEmptyString(raw.originAddress);
    const defaultRouteMode = readRouteMode(raw.defaultRouteMode);
    const arrivalBufferMinutes = readPositiveNumber(raw.arrivalBufferMinutes);
    const weather = readQWeatherConfig(raw.weather);
    if (originAddress)
        config.originAddress = originAddress;
    if (defaultRouteMode)
        config.defaultRouteMode = defaultRouteMode;
    if (arrivalBufferMinutes !== undefined)
        config.arrivalBufferMinutes = Math.floor(arrivalBufferMinutes);
    if (weather)
        config.weather = weather;
    return Object.keys(config).length > 0 ? config : undefined;
}
function readQWeatherConfig(value) {
    const raw = typeof value === "object" && value !== null && !Array.isArray(value) ? value : {};
    if (raw.provider !== "qweather")
        return undefined;
    const config = {
        provider: "qweather"
    };
    const apiHost = readNonEmptyString(raw.apiHost);
    const credentialEnv = readNonEmptyString(raw.credentialEnv);
    const authMode = readQWeatherAuthMode(raw.authMode);
    const defaultLanguage = readNonEmptyString(raw.defaultLanguage);
    const defaultUnit = readQWeatherUnit(raw.defaultUnit);
    const defaultRange = readNonEmptyString(raw.defaultRange);
    const forecastDays = readQWeatherForecastDays(raw.forecastDays);
    const timeoutMs = readPositiveNumber(raw.timeoutMs);
    if (apiHost)
        config.apiHost = apiHost;
    if (credentialEnv)
        config.credentialEnv = credentialEnv;
    if (authMode)
        config.authMode = authMode;
    if (defaultLanguage)
        config.defaultLanguage = defaultLanguage;
    if (defaultUnit)
        config.defaultUnit = defaultUnit;
    if (defaultRange)
        config.defaultRange = defaultRange;
    if (forecastDays)
        config.forecastDays = forecastDays;
    if (timeoutMs !== undefined)
        config.timeoutMs = timeoutMs;
    return config;
}
function readWellbeingConfig(value) {
    const raw = typeof value === "object" && value !== null && !Array.isArray(value) ? value : {};
    const config = {};
    const minimumSedentaryDurationMinutes = readPositiveNumber(raw.minimumSedentaryDurationMinutes);
    const minimumConfidence = readBoundedNumber(raw.minimumConfidence, 0, 1);
    const cooldownMinutes = readPositiveNumber(raw.cooldownMinutes);
    const upcomingReminderHorizonMinutes = readPositiveNumber(raw.upcomingReminderHorizonMinutes);
    if (minimumSedentaryDurationMinutes !== undefined) {
        config.minimumSedentaryDurationMinutes = minimumSedentaryDurationMinutes;
    }
    if (minimumConfidence !== undefined)
        config.minimumConfidence = minimumConfidence;
    if (cooldownMinutes !== undefined)
        config.cooldownMinutes = cooldownMinutes;
    if (upcomingReminderHorizonMinutes !== undefined) {
        config.upcomingReminderHorizonMinutes = upcomingReminderHorizonMinutes;
    }
    return Object.keys(config).length > 0 ? config : undefined;
}
function readPositiveNumber(value) {
    return typeof value === "number" && Number.isFinite(value) && value > 0 ? value : undefined;
}
function readBoundedNumber(value, min, max) {
    return typeof value === "number" && Number.isFinite(value) && value >= min && value <= max ? value : undefined;
}
function readNonEmptyString(value) {
    return typeof value === "string" && value.trim() !== "" ? value.trim() : undefined;
}
function readRouteMode(value) {
    return value === "driving" || value === "transit" || value === "walking" ? value : undefined;
}
function readQWeatherAuthMode(value) {
    return value === "apiKeyHeader" || value === "apiKeyQuery" || value === "jwtBearer" ? value : undefined;
}
function readQWeatherUnit(value) {
    return value === "metric" || value === "imperial" ? value : undefined;
}
function readQWeatherForecastDays(value) {
    return value === 3 || value === 7 || value === 10 || value === 15 || value === 30 ? value : undefined;
}
function startSchedulerLoop(scheduler, config) {
    let stopped = false;
    let running = false;
    const run = () => {
        if (stopped)
            return;
        if (running)
            return;
        running = true;
        void scheduler
            .tick(new Date())
            .catch((error) => {
            console.error("work-assistant scheduler tick failed", error);
        })
            .finally(() => {
            running = false;
        });
    };
    run();
    const timer = setInterval(run, config.scanIntervalMs);
    timer.unref?.();
    return {
        stop() {
            stopped = true;
            clearInterval(timer);
        }
    };
}
function registerDisposal(api, callback) {
    if (typeof api.onDispose === "function")
        api.onDispose(callback);
}
//# sourceMappingURL=index.js.map