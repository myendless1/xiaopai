const DEFAULT_SCAN_INTERVAL_MS = 10 * 60 * 1000;
const DEFAULT_LOOKAHEAD_HOURS = 48;
const DEFAULT_TIMEZONE = "Asia/Shanghai";
const DEFAULT_USER_ID = "ou_requester";
const DEFAULT_CALENDAR_ID = "primary";
const DEFAULT_MAX_DISPATCH_ATTEMPTS = 3;
const DEFAULT_AGENT_DISPATCH_DELIVERY_MODE = "none";
export const DEFAULT_SCHEDULER_RULES = {
    dailyBriefing: {
        enabled: true,
        localTime: "08:00"
    },
    meetingStartingSoon: {
        enabled: false,
        offsetMinutes: 10,
        keywords: ["meeting", "sync", "review", "会议", "同步", "评审", "周会", "例会"]
    },
    outdoorEvent: {
        enabled: false,
        offsetMinutes: 60,
        keywords: ["外出", "户外", "拜访", "客户", "园区", "site", "field", "customer", "client"]
    },
    businessTripTomorrow: {
        enabled: false,
        localTime: "18:00",
        keywords: ["出差", "差旅", "航班", "高铁", "火车", "酒店", "travel", "trip", "flight", "train"]
    }
};
export function readSchedulerConfig(value) {
    const raw = isRecord(value) ? value : {};
    const enabled = raw.enabled === true;
    const statePath = readNonEmptyString(raw.statePath);
    return {
        enabled,
        startIntervalLoop: raw.startIntervalLoop === true,
        scanIntervalMs: readPositiveNumber(raw.scanIntervalMs) ?? DEFAULT_SCAN_INTERVAL_MS,
        lookaheadHours: readPositiveNumber(raw.lookaheadHours) ?? DEFAULT_LOOKAHEAD_HOURS,
        timezone: readNonEmptyString(raw.timezone) ?? DEFAULT_TIMEZONE,
        userId: readNonEmptyString(raw.userId) ?? DEFAULT_USER_ID,
        calendarId: readNonEmptyString(raw.calendarId) ?? DEFAULT_CALENDAR_ID,
        ...(statePath ? { statePath } : {}),
        maxDispatchAttempts: Math.floor(readPositiveNumber(raw.maxDispatchAttempts) ?? DEFAULT_MAX_DISPATCH_ATTEMPTS),
        agentDispatch: readAgentDispatchConfig(raw.agentDispatch),
        rules: readRulesConfig(raw.rules)
    };
}
function readAgentDispatchConfig(value) {
    const raw = isRecord(value) ? value : {};
    const sessionKey = readNonEmptyString(raw.sessionKey);
    const sessionKeyMode = raw.sessionKeyMode === "online_xiaopai" ? "online_xiaopai" : undefined;
    const agentId = readNonEmptyString(raw.agentId);
    const deviceId = readNonEmptyString(raw.deviceId);
    const xiaopaiBaseUrl = readNonEmptyString(raw.xiaopaiBaseUrl);
    const xiaopaiDeviceLookupTimeoutMs = readPositiveNumber(raw.xiaopaiDeviceLookupTimeoutMs);
    const deliveryMode = raw.deliveryMode === "announce" || raw.deliveryMode === "none"
        ? raw.deliveryMode
        : DEFAULT_AGENT_DISPATCH_DELIVERY_MODE;
    return {
        enabled: raw.enabled === true,
        ...(sessionKey ? { sessionKey } : {}),
        ...(sessionKeyMode ? { sessionKeyMode } : {}),
        ...(agentId ? { agentId } : {}),
        deliveryMode,
        ...(deviceId ? { deviceId } : {}),
        ...(xiaopaiBaseUrl ? { xiaopaiBaseUrl } : {}),
        ...(xiaopaiDeviceLookupTimeoutMs ? { xiaopaiDeviceLookupTimeoutMs } : {}),
        interrupt: typeof raw.interrupt === "boolean" ? raw.interrupt : true
    };
}
function readRulesConfig(value) {
    const raw = isRecord(value) ? value : {};
    const dailyBriefing = isRecord(raw.dailyBriefing) ? raw.dailyBriefing : {};
    const meetingStartingSoon = isRecord(raw.meetingStartingSoon) ? raw.meetingStartingSoon : {};
    const outdoorEvent = isRecord(raw.outdoorEvent) ? raw.outdoorEvent : {};
    const businessTripTomorrow = isRecord(raw.businessTripTomorrow) ? raw.businessTripTomorrow : {};
    return {
        dailyBriefing: {
            enabled: readBoolean(dailyBriefing.enabled, DEFAULT_SCHEDULER_RULES.dailyBriefing.enabled),
            localTime: readLocalTime(dailyBriefing.localTime) ?? DEFAULT_SCHEDULER_RULES.dailyBriefing.localTime
        },
        meetingStartingSoon: {
            enabled: readBoolean(meetingStartingSoon.enabled, DEFAULT_SCHEDULER_RULES.meetingStartingSoon.enabled),
            offsetMinutes: readPositiveNumber(meetingStartingSoon.offsetMinutes) ??
                DEFAULT_SCHEDULER_RULES.meetingStartingSoon.offsetMinutes,
            keywords: readStringArray(meetingStartingSoon.keywords) ?? DEFAULT_SCHEDULER_RULES.meetingStartingSoon.keywords
        },
        outdoorEvent: {
            enabled: readBoolean(outdoorEvent.enabled, DEFAULT_SCHEDULER_RULES.outdoorEvent.enabled),
            offsetMinutes: readPositiveNumber(outdoorEvent.offsetMinutes) ?? DEFAULT_SCHEDULER_RULES.outdoorEvent.offsetMinutes,
            keywords: readStringArray(outdoorEvent.keywords) ?? DEFAULT_SCHEDULER_RULES.outdoorEvent.keywords
        },
        businessTripTomorrow: {
            enabled: readBoolean(businessTripTomorrow.enabled, DEFAULT_SCHEDULER_RULES.businessTripTomorrow.enabled),
            localTime: readLocalTime(businessTripTomorrow.localTime) ??
                DEFAULT_SCHEDULER_RULES.businessTripTomorrow.localTime,
            keywords: readStringArray(businessTripTomorrow.keywords) ??
                DEFAULT_SCHEDULER_RULES.businessTripTomorrow.keywords
        }
    };
}
function isRecord(value) {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}
function readNonEmptyString(value) {
    return typeof value === "string" && value.trim() !== "" ? value.trim() : undefined;
}
function readPositiveNumber(value) {
    return typeof value === "number" && Number.isFinite(value) && value > 0 ? value : undefined;
}
function readBoolean(value, fallback) {
    return typeof value === "boolean" ? value : fallback;
}
function readStringArray(value) {
    if (!Array.isArray(value))
        return undefined;
    const items = value.filter((item) => typeof item === "string" && item.trim() !== "");
    return items.length > 0 ? [...new Set(items.map((item) => item.trim()))] : undefined;
}
function readLocalTime(value) {
    if (typeof value !== "string")
        return undefined;
    const trimmed = value.trim();
    return /^([01]\d|2[0-3]):[0-5]\d$/.test(trimmed) ? trimmed : undefined;
}
//# sourceMappingURL=config.js.map