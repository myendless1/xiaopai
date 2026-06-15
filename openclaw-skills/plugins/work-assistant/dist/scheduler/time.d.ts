export type LocalDateParts = {
    year: number;
    month: number;
    day: number;
};
export type LocalTimeOfDay = {
    hour: number;
    minute: number;
};
export declare function calculateScanWindow(now: Date, lookaheadHours: number): {
    start: string;
    end: string;
};
export declare function getLocalDateParts(timestamp: string | Date, timezone: string): LocalDateParts;
export declare function addLocalDays(parts: LocalDateParts, days: number): LocalDateParts;
export declare function formatLocalDate(parts: LocalDateParts): string;
export declare function parseLocalTime(value: string | undefined, fallback: LocalTimeOfDay): LocalTimeOfDay;
export declare function zonedTimeToUtcIso(date: LocalDateParts, time: LocalTimeOfDay, timezone: string, second?: number, millisecond?: number): string;
export declare function isSameLocalDate(timestamp: string, date: LocalDateParts, timezone: string): boolean;
export declare function addMinutes(timestamp: string, minutes: number): string;
export declare function addDaysIso(timestamp: string, days: number): string;
