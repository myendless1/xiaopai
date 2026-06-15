import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname } from "node:path";
import type {
  StructuredResponse
} from "../contracts.js";
import type {
  TriggerPlan,
  TriggerPlanStore,
  TriggerStoreRecord,
  TriggerStoreSnapshot,
  TriggerUpsertResult
} from "./types.js";

export class MemoryTriggerPlanStore implements TriggerPlanStore {
  protected readonly records = new Map<string, TriggerStoreRecord>();
  protected readonly claimedKeys: Set<string>;

  constructor(claimedKeys = new Set<string>()) {
    this.claimedKeys = claimedKeys;
  }

  async upsertPlans(plans: TriggerPlan[], now: string): Promise<TriggerUpsertResult> {
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
      if (existing?.status === "dispatched") continue;
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

  async getDue(now: string): Promise<TriggerStoreRecord[]> {
    const nowMs = Date.parse(now);
    const due = [...this.records.values()]
      .filter(
        (record) =>
          record.status === "pending" &&
          Date.parse(record.scheduledFor) <= nowMs &&
          record.attempts < record.maxAttempts &&
          !this.claimedKeys.has(record.key)
      )
      .sort((left, right) => Date.parse(left.scheduledFor) - Date.parse(right.scheduledFor));
    for (const record of due) this.claimedKeys.add(record.key);
    return due;
  }

  async markDispatched(key: string, eventId: string, dispatchedAt: string, response: StructuredResponse): Promise<void> {
    const record = this.records.get(key);
    if (!record) return;
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

  async recordDispatchFailure(key: string, error: string, failedAt: string): Promise<void> {
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

  async listRecords(): Promise<TriggerStoreRecord[]> {
    return [...this.records.values()].sort((left, right) => left.key.localeCompare(right.key));
  }

  protected async persist(): Promise<void> {
    return;
  }
}

export class JsonFileTriggerPlanStore extends MemoryTriggerPlanStore {
  private static readonly claimsByStatePath = new Map<string, Set<string>>();
  private loaded = false;

  constructor(private readonly statePath: string) {
    super(JsonFileTriggerPlanStore.claimSetFor(statePath));
  }

  override async upsertPlans(plans: TriggerPlan[], now: string): Promise<TriggerUpsertResult> {
    await this.load({ force: true });
    return super.upsertPlans(plans, now);
  }

  override async getDue(now: string): Promise<TriggerStoreRecord[]> {
    await this.load({ force: true });
    return super.getDue(now);
  }

  override async markDispatched(key: string, eventId: string, dispatchedAt: string, response: StructuredResponse): Promise<void> {
    await this.load({ force: true });
    return super.markDispatched(key, eventId, dispatchedAt, response);
  }

  override async recordDispatchFailure(key: string, error: string, failedAt: string): Promise<void> {
    await this.load({ force: true });
    return super.recordDispatchFailure(key, error, failedAt);
  }

  override async listRecords(): Promise<TriggerStoreRecord[]> {
    await this.load({ force: true });
    return super.listRecords();
  }

  protected override async persist(): Promise<void> {
    if (!this.loaded) return;
    await mkdir(dirname(this.statePath), { recursive: true });
    const snapshot: TriggerStoreSnapshot = {
      version: 1,
      records: [...this.records.values()]
    };
    await writeFile(this.statePath, `${JSON.stringify(snapshot, null, 2)}\n`, "utf8");
  }

  private async load(options: { force?: boolean } = {}): Promise<void> {
    if (this.loaded && !options.force) return;
    this.loaded = true;
    if (options.force) this.records.clear();
    try {
      const raw = await readFile(this.statePath, "utf8");
      const parsed = JSON.parse(raw) as TriggerStoreSnapshot;
      if (parsed.version !== 1 || !Array.isArray(parsed.records)) return;
      for (const record of parsed.records) {
        if (isTriggerStoreRecord(record)) this.records.set(record.key, record);
      }
    } catch (error) {
      if (error instanceof Error && "code" in error && error.code === "ENOENT") return;
      throw error;
    }
  }

  private static claimSetFor(statePath: string): Set<string> {
    const existing = JsonFileTriggerPlanStore.claimsByStatePath.get(statePath);
    if (existing) return existing;
    const claims = new Set<string>();
    JsonFileTriggerPlanStore.claimsByStatePath.set(statePath, claims);
    return claims;
  }
}

function isTriggerStoreRecord(value: unknown): value is TriggerStoreRecord {
  if (typeof value !== "object" || value === null || Array.isArray(value)) return false;
  const record = value as Record<string, unknown>;
  return (
    typeof record.key === "string" &&
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
    typeof record.attempts === "number"
  );
}
