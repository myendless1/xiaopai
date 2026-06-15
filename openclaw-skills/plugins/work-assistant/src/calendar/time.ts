export type ResolvedDate = {
  year: number;
  month: number;
  day: number;
};

export type TimeOfDay = {
  hour: number;
  minute: number;
};

function dateParts(timestamp: string, timezone: string): ResolvedDate {
  const formatter = new Intl.DateTimeFormat("en-CA", {
    timeZone: timezone,
    year: "numeric",
    month: "2-digit",
    day: "2-digit"
  });
  const parts = Object.fromEntries(formatter.formatToParts(new Date(timestamp)).map((part) => [part.type, part.value]));
  return {
    year: Number(parts.year),
    month: Number(parts.month),
    day: Number(parts.day)
  };
}

function addDays(date: ResolvedDate, days: number): ResolvedDate {
  const utc = Date.UTC(date.year, date.month - 1, date.day + days);
  const next = new Date(utc);
  return {
    year: next.getUTCFullYear(),
    month: next.getUTCMonth() + 1,
    day: next.getUTCDate()
  };
}

function offsetFor(timezone: string, date: ResolvedDate, time: TimeOfDay): string {
  if (timezone === "Asia/Shanghai" || timezone === "Asia/Chongqing" || timezone === "Asia/Harbin") {
    return "+08:00";
  }
  const probe = new Date(Date.UTC(date.year, date.month - 1, date.day, time.hour, time.minute));
  const formatter = new Intl.DateTimeFormat("en-US", {
    timeZone: timezone,
    timeZoneName: "shortOffset",
    hour: "2-digit"
  });
  const tzName = formatter.formatToParts(probe).find((part) => part.type === "timeZoneName")?.value ?? "GMT+0";
  const match = /^GMT([+-])(\d{1,2})(?::?(\d{2}))?$/.exec(tzName);
  if (!match) return "Z";
  const sign = match[1] ?? "+";
  const hour = match[2] ?? "0";
  const minute = match[3] ?? "00";
  return `${sign}${hour.padStart(2, "0")}:${minute}`;
}

function pad(value: number): string {
  return String(value).padStart(2, "0");
}

export function resolveRelativeDate(keyword: string, eventTimestamp: string, timezone: string): ResolvedDate | undefined {
  const base = dateParts(eventTimestamp, timezone);
  if (keyword === "今天") return base;
  if (keyword === "明天") return addDays(base, 1);
  if (keyword === "后天") return addDays(base, 2);
  return undefined;
}

export function localIso(date: ResolvedDate, time: TimeOfDay, timezone: string): string {
  return `${date.year}-${pad(date.month)}-${pad(date.day)}T${pad(time.hour)}:${pad(time.minute)}:00${offsetFor(timezone, date, time)}`;
}

export function compareIso(a: string, b: string): number {
  return Date.parse(a) - Date.parse(b);
}
