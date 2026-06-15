const HOUR_MS = 60 * 60 * 1000;
const DAY_MS = 24 * HOUR_MS;
export function calculateScanWindow(now, lookaheadHours) {
    if (!Number.isFinite(now.getTime()))
        throw new Error("Invalid scheduler timestamp.");
    return {
        start: now.toISOString(),
        end: new Date(now.getTime() + lookaheadHours * HOUR_MS).toISOString()
    };
}
export function getLocalDateParts(timestamp, timezone) {
    const date = timestamp instanceof Date ? timestamp : new Date(timestamp);
    if (!Number.isFinite(date.getTime()))
        throw new Error("Invalid timestamp.");
    const formatter = new Intl.DateTimeFormat("en-US", {
        timeZone: timezone,
        year: "numeric",
        month: "2-digit",
        day: "2-digit"
    });
    const parts = Object.fromEntries(formatter.formatToParts(date).map((part) => [part.type, part.value]));
    return {
        year: Number(parts.year),
        month: Number(parts.month),
        day: Number(parts.day)
    };
}
export function addLocalDays(parts, days) {
    const date = new Date(Date.UTC(parts.year, parts.month - 1, parts.day + days, 12, 0, 0, 0));
    return {
        year: date.getUTCFullYear(),
        month: date.getUTCMonth() + 1,
        day: date.getUTCDate()
    };
}
export function formatLocalDate(parts) {
    return `${parts.year}-${String(parts.month).padStart(2, "0")}-${String(parts.day).padStart(2, "0")}`;
}
export function parseLocalTime(value, fallback) {
    if (!value)
        return fallback;
    const match = /^([01]?\d|2[0-3]):([0-5]\d)$/.exec(value.trim());
    if (!match)
        return fallback;
    return {
        hour: Number(match[1]),
        minute: Number(match[2])
    };
}
export function zonedTimeToUtcIso(date, time, timezone, second = 0, millisecond = 0) {
    const localAsUtc = Date.UTC(date.year, date.month - 1, date.day, time.hour, time.minute, second, millisecond);
    let utc = localAsUtc - getTimezoneOffsetMs(localAsUtc, timezone);
    utc = localAsUtc - getTimezoneOffsetMs(utc, timezone);
    return new Date(utc).toISOString();
}
export function isSameLocalDate(timestamp, date, timezone) {
    const parts = getLocalDateParts(timestamp, timezone);
    return parts.year === date.year && parts.month === date.month && parts.day === date.day;
}
export function addMinutes(timestamp, minutes) {
    return new Date(Date.parse(timestamp) + minutes * 60 * 1000).toISOString();
}
export function addDaysIso(timestamp, days) {
    return new Date(Date.parse(timestamp) + days * DAY_MS).toISOString();
}
function getTimezoneOffsetMs(instantMs, timezone) {
    const formatter = new Intl.DateTimeFormat("en-US", {
        timeZone: timezone,
        year: "numeric",
        month: "2-digit",
        day: "2-digit",
        hour: "2-digit",
        minute: "2-digit",
        second: "2-digit",
        hour12: false
    });
    const parts = Object.fromEntries(formatter.formatToParts(new Date(instantMs)).map((part) => [part.type, part.value]));
    const localAsUtc = Date.UTC(Number(parts.year), Number(parts.month) - 1, Number(parts.day), Number(parts.hour) % 24, Number(parts.minute), Number(parts.second), 0);
    return localAsUtc - instantMs;
}
//# sourceMappingURL=time.js.map