import { createHash } from "node:crypto";
import type { ProactiveCalendarRuleId } from "./types.js";

export function stableJson(value: unknown): string {
  if (Array.isArray(value)) return `[${value.map((item) => stableJson(item)).join(",")}]`;
  if (value && typeof value === "object") {
    const record = value as Record<string, unknown>;
    return `{${Object.keys(record)
      .sort()
      .map((key) => `${JSON.stringify(key)}:${stableJson(record[key])}`)
      .join(",")}}`;
  }
  return JSON.stringify(value);
}

export function stableHash(value: unknown): string {
  return createHash("sha256").update(stableJson(value)).digest("hex").slice(0, 32);
}

export function deriveTriggerKey(input: {
  userId: string;
  calendarId: string;
  sourceEventId?: string;
  ruleId: ProactiveCalendarRuleId;
  scheduledFor: string;
}): string {
  return `trigger_${stableHash(input)}`;
}

export function deriveTriggerUpdateGroupKey(input: {
  userId: string;
  calendarId: string;
  sourceEventId: string;
  ruleId: ProactiveCalendarRuleId;
}): string {
  return `trigger_group_${stableHash(input)}`;
}

export function deriveInputEventId(triggerKey: string): string {
  return `proactive_${stableHash({ triggerKey })}`;
}
