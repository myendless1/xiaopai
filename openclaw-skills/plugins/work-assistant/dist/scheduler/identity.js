import { createHash } from "node:crypto";
export function stableJson(value) {
    if (Array.isArray(value))
        return `[${value.map((item) => stableJson(item)).join(",")}]`;
    if (value && typeof value === "object") {
        const record = value;
        return `{${Object.keys(record)
            .sort()
            .map((key) => `${JSON.stringify(key)}:${stableJson(record[key])}`)
            .join(",")}}`;
    }
    return JSON.stringify(value);
}
export function stableHash(value) {
    return createHash("sha256").update(stableJson(value)).digest("hex").slice(0, 32);
}
export function deriveTriggerKey(input) {
    return `trigger_${stableHash(input)}`;
}
export function deriveTriggerUpdateGroupKey(input) {
    return `trigger_group_${stableHash(input)}`;
}
export function deriveInputEventId(triggerKey) {
    return `proactive_${stableHash({ triggerKey })}`;
}
//# sourceMappingURL=identity.js.map