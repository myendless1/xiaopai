export type ResolvedDate = {
    year: number;
    month: number;
    day: number;
};
export type TimeOfDay = {
    hour: number;
    minute: number;
};
export declare function resolveRelativeDate(keyword: string, eventTimestamp: string, timezone: string): ResolvedDate | undefined;
export declare function localIso(date: ResolvedDate, time: TimeOfDay, timezone: string): string;
export declare function compareIso(a: string, b: string): number;
