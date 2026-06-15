import type { StructuredResponse } from "../contracts.js";
import type { TriggerPlan, TriggerPlanStore, TriggerStoreRecord, TriggerUpsertResult } from "./types.js";
export declare class MemoryTriggerPlanStore implements TriggerPlanStore {
    protected readonly records: Map<string, TriggerStoreRecord>;
    protected readonly claimedKeys: Set<string>;
    constructor(claimedKeys?: Set<string>);
    upsertPlans(plans: TriggerPlan[], now: string): Promise<TriggerUpsertResult>;
    getDue(now: string): Promise<TriggerStoreRecord[]>;
    markDispatched(key: string, eventId: string, dispatchedAt: string, response: StructuredResponse): Promise<void>;
    recordDispatchFailure(key: string, error: string, failedAt: string): Promise<void>;
    listRecords(): Promise<TriggerStoreRecord[]>;
    protected persist(): Promise<void>;
}
export declare class JsonFileTriggerPlanStore extends MemoryTriggerPlanStore {
    private readonly statePath;
    private static readonly claimsByStatePath;
    private loaded;
    constructor(statePath: string);
    upsertPlans(plans: TriggerPlan[], now: string): Promise<TriggerUpsertResult>;
    getDue(now: string): Promise<TriggerStoreRecord[]>;
    markDispatched(key: string, eventId: string, dispatchedAt: string, response: StructuredResponse): Promise<void>;
    recordDispatchFailure(key: string, error: string, failedAt: string): Promise<void>;
    listRecords(): Promise<TriggerStoreRecord[]>;
    protected persist(): Promise<void>;
    private load;
    private static claimSetFor;
}
