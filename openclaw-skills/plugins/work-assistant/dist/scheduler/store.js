import { mkdir, readFile, rename, writeFile } from "node:fs/promises";
import { dirname } from "node:path";
export class MemoryTriggerPlanStore {
    records = new Map();
    claimedKeys;
    constructor(claimedKeys = new Set()) {
        this.claimedKeys = claimedKeys;
    }
    async upsertPlans(plans, now) {
        let upserted = 0;
        let replacedPending = 0;
        for (const plan of plans) {
            for (const [key, record] of this.records) {
                if (record.status === "pending" && record.updateGroupKey === plan.updateGroupKey && key !== plan.key) {
                    this.records.delete(key);
                    replacedPending += 1;
                }
            }
            const existing = this.records.get(plan.key);
            if (existing?.status === "dispatched")
                continue;
            this.records.set(plan.key, {
                ...plan,
                status: "pending",
                createdAt: existing?.createdAt ?? now,
                updatedAt: now,
                attempts: existing?.attempts ?? 0,
                ...(existing?.lastDispatchError ? { lastDispatchError: existing.lastDispatchError } : {})
            });
            upserted += 1;
        }
        await this.persist();
        return { upserted, replacedPending };
    }
    async getDue(now) {
        const nowMs = Date.parse(now);
        const due = [...this.records.values()]
            .filter((record) => record.status === "pending" &&
            Date.parse(record.scheduledFor) <= nowMs &&
            record.attempts < record.maxAttempts &&
            !this.claimedKeys.has(record.key))
            .sort((left, right) => Date.parse(left.scheduledFor) - Date.parse(right.scheduledFor));
        for (const record of due)
            this.claimedKeys.add(record.key);
        return due;
    }
    async markDispatched(key, eventId, dispatchedAt, response) {
        const record = this.records.get(key);
        if (!record)
            return;
        this.records.set(key, {
            ...record,
            status: "dispatched",
            updatedAt: dispatchedAt,
            dispatchedAt,
            lastEventId: eventId,
            responseSummary: {
                speech: response.speech,
                actionCount: response.actions.length,
                followUpExpected: response.follow_up.expected
            }
        });
        await this.persist();
    }
    async recordDispatchFailure(key, error, failedAt) {
        const record = this.records.get(key);
        if (!record) {
            this.claimedKeys.delete(key);
            return;
        }
        this.records.set(key, {
            ...record,
            updatedAt: failedAt,
            attempts: record.attempts + 1,
            lastDispatchError: error
        });
        this.claimedKeys.delete(key);
        await this.persist();
    }
    async listRecords() {
        return [...this.records.values()].sort((left, right) => left.key.localeCompare(right.key));
    }
    async persist() {
        return;
    }
}
export class JsonFileTriggerPlanStore extends MemoryTriggerPlanStore {
    statePath;
    static claimsByStatePath = new Map();
    static operationChainsByStatePath = new Map();
    loaded = false;
    constructor(statePath) {
        super(JsonFileTriggerPlanStore.claimSetFor(statePath));
        this.statePath = statePath;
    }
    async upsertPlans(plans, now) {
        return this.runExclusive(async () => {
            await this.load({ force: true });
            return super.upsertPlans(plans, now);
        });
    }
    async getDue(now) {
        return this.runExclusive(async () => {
            await this.load({ force: true });
            return super.getDue(now);
        });
    }
    async markDispatched(key, eventId, dispatchedAt, response) {
        return this.runExclusive(async () => {
            await this.load({ force: true });
            return super.markDispatched(key, eventId, dispatchedAt, response);
        });
    }
    async recordDispatchFailure(key, error, failedAt) {
        return this.runExclusive(async () => {
            await this.load({ force: true });
            return super.recordDispatchFailure(key, error, failedAt);
        });
    }
    async listRecords() {
        return this.runExclusive(async () => {
            await this.load({ force: true });
            return super.listRecords();
        });
    }
    async persist() {
        if (!this.loaded)
            return;
        await mkdir(dirname(this.statePath), { recursive: true });
        const snapshot = {
            version: 1,
            records: [...this.records.values()]
        };
        const tempPath = `${this.statePath}.${process.pid}.${Date.now()}.${Math.random().toString(36).slice(2)}.tmp`;
        await writeFile(tempPath, `${JSON.stringify(snapshot, null, 2)}\n`, "utf8");
        await rename(tempPath, this.statePath);
    }
    async load(options = {}) {
        if (this.loaded && !options.force)
            return;
        this.loaded = true;
        if (options.force)
            this.records.clear();
        try {
            const raw = await readFile(this.statePath, "utf8");
            const parsed = JSON.parse(raw);
            if (parsed.version !== 1 || !Array.isArray(parsed.records))
                return;
            for (const record of parsed.records) {
                if (isTriggerStoreRecord(record))
                    this.records.set(record.key, record);
            }
        }
        catch (error) {
            if (error instanceof Error && "code" in error && error.code === "ENOENT")
                return;
            throw error;
        }
    }
    static claimSetFor(statePath) {
        const existing = JsonFileTriggerPlanStore.claimsByStatePath.get(statePath);
        if (existing)
            return existing;
        const claims = new Set();
        JsonFileTriggerPlanStore.claimsByStatePath.set(statePath, claims);
        return claims;
    }
    async runExclusive(operation) {
        const previous = JsonFileTriggerPlanStore.operationChainsByStatePath.get(this.statePath) ?? Promise.resolve();
        let release = () => undefined;
        const gate = new Promise((resolve) => {
            release = resolve;
        });
        const chain = previous.catch(() => undefined).then(() => gate);
        JsonFileTriggerPlanStore.operationChainsByStatePath.set(this.statePath, chain);
        await previous.catch(() => undefined);
        try {
            return await operation();
        }
        finally {
            release();
            if (JsonFileTriggerPlanStore.operationChainsByStatePath.get(this.statePath) === chain) {
                void chain.finally(() => {
                    if (JsonFileTriggerPlanStore.operationChainsByStatePath.get(this.statePath) === chain) {
                        JsonFileTriggerPlanStore.operationChainsByStatePath.delete(this.statePath);
                    }
                });
            }
        }
    }
}
function isTriggerStoreRecord(value) {
    if (typeof value !== "object" || value === null || Array.isArray(value))
        return false;
    const record = value;
    return (typeof record.key === "string" &&
        typeof record.updateGroupKey === "string" &&
        typeof record.eventId === "string" &&
        typeof record.ruleId === "string" &&
        typeof record.type === "string" &&
        typeof record.userId === "string" &&
        typeof record.calendarId === "string" &&
        typeof record.scheduledFor === "string" &&
        typeof record.eventHash === "string" &&
        typeof record.maxAttempts === "number" &&
        (record.status === "pending" || record.status === "dispatched") &&
        typeof record.createdAt === "string" &&
        typeof record.updatedAt === "string" &&
        typeof record.attempts === "number");
}
//# sourceMappingURL=store.js.map