import type { StructuredResponse } from "../contracts.js";
import type { LarkCalendarAdapter } from "../lark/adapters.js";
import type { ProactiveCalendarDispatchResult, ProactiveCalendarScanResult, ProactiveCalendarSchedulerConfig, SchedulerDispatchCallback, SchedulerDispatchCallbackResult, TriggerPlanStore, TriggerStoreRecord } from "./types.js";
export type ProactiveCalendarTriggerSchedulerOptions = {
    config: ProactiveCalendarSchedulerConfig;
    calendarAdapter: LarkCalendarAdapter;
    store: TriggerPlanStore;
    dispatch: SchedulerDispatchCallback;
};
export declare class ProactiveCalendarTriggerScheduler {
    private readonly config;
    private readonly calendarAdapter;
    private readonly store;
    private readonly dispatch;
    constructor(options: ProactiveCalendarTriggerSchedulerOptions);
    refresh(now?: Date): Promise<ProactiveCalendarScanResult>;
    dispatchDue(now?: Date): Promise<ProactiveCalendarDispatchResult[]>;
    tick(now?: Date): Promise<{
        scan: ProactiveCalendarScanResult;
        dispatches: ProactiveCalendarDispatchResult[];
    }>;
    listRecords(): Promise<TriggerStoreRecord[]>;
    private toInputEvent;
}
export declare function unwrapDispatchResponse(result: StructuredResponse | SchedulerDispatchCallbackResult): SchedulerDispatchCallbackResult;
