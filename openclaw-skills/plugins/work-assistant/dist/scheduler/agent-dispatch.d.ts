import type { InputEvent, StructuredResponse } from "../contracts.js";
import type { ProactiveCalendarAgentDispatchConfig } from "./types.js";
export declare const STACKCHAN_EVENT_SCHEMA = "openclaw.stackchan.event.v1";
export declare const WORK_ASSISTANT_SCHEDULER_RESPONSE_SCHEMA = "openclaw.work_assistant.scheduler_response.v1";
export type SchedulerAgentTurnScheduleParams = {
    sessionKey: string;
    message: string;
    delayMs: number;
    deleteAfterRun: boolean;
    deliveryMode: "none" | "announce";
    tag: string;
    name: string;
    agentId?: string;
};
export type SchedulerAgentTurnHandle = {
    id: string;
    pluginId?: string;
    sessionKey: string;
    kind: string;
};
export type SchedulerAgentTurnScheduler = (params: SchedulerAgentTurnScheduleParams) => Promise<SchedulerAgentTurnHandle | undefined>;
export type SchedulerAgentDispatchRuntimeApi = {
    session?: {
        workflow?: {
            scheduleSessionTurn?: SchedulerAgentTurnScheduler;
        };
    };
    scheduleSessionTurn?: SchedulerAgentTurnScheduler;
    runAgentTurnCli?: SchedulerAgentTurnScheduler;
    resolveOnlineXiaopaiDeviceId?: (config: ProactiveCalendarAgentDispatchConfig) => Promise<string | undefined>;
    logger?: {
        info?: (message: string) => void;
        warn?: (message: string) => void;
    };
};
export type SchedulerAgentDispatchResult = {
    status: "success";
    jobId: string;
    sessionKey: string;
} | {
    status: "skipped";
    reason: "disabled" | "missing_speech";
} | {
    status: "failed";
    code: string;
    message: string;
};
export declare function buildSchedulerAgentTurnMessage(options: {
    event: InputEvent;
    response: StructuredResponse;
    config: ProactiveCalendarAgentDispatchConfig;
}): string;
export declare function dispatchSchedulerResponseToAgent(options: {
    api: SchedulerAgentDispatchRuntimeApi;
    event: InputEvent;
    response: StructuredResponse;
    config: ProactiveCalendarAgentDispatchConfig;
}): Promise<SchedulerAgentDispatchResult>;
export declare function buildDynamicXiaopaiSessionKey(config: Pick<ProactiveCalendarAgentDispatchConfig, "sessionKey" | "agentId">, deviceId: string): string;
export declare function selectOnlineXiaopaiDeviceId(value: unknown): string | undefined;
export declare function scheduleAgentTurnWithOpenClawCron(params: SchedulerAgentTurnScheduleParams): Promise<SchedulerAgentTurnHandle | undefined>;
export declare function buildOpenClawCronAddArgs(params: SchedulerAgentTurnScheduleParams): string[];
