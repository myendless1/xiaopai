import { localIso, resolveRelativeDate, type TimeOfDay } from "./time.js";
import type { InputEvent } from "../contracts.js";

export type CalendarCreateIntent = {
  title?: string;
  attendeeNames: string[];
  start?: string;
  end?: string;
  ambiguity?: string;
  confidence: number;
};

export class CalendarIntentParser {
  parse(event: InputEvent): CalendarCreateIntent {
    const text = typeof event.payload.text === "string" ? event.payload.text.trim() : "";
    if (!text) return { attendeeNames: [], confidence: 0, ambiguity: "missing_text" };
    if (!/(建|创建|安排|预约).*(飞书)?(日程|会议|会)/.test(text)) {
      return { attendeeNames: [], confidence: 0.2, ambiguity: "unsupported_utterance" };
    }

    const relativeDate = /(今天|明天|后天)/.exec(text)?.[1];
    const date = relativeDate ? resolveRelativeDate(relativeDate, event.timestamp, event.context.timezone) : undefined;
    const times = this.extractTimes(text);
    const title = this.extractTitle(text);
    const attendeeNames = this.extractAttendees(text);

    const intent: CalendarCreateIntent = {
      attendeeNames,
      confidence: title && date && times ? 0.92 : 0.5
    };
    if (title) intent.title = title;
    if (date && times?.start) intent.start = localIso(date, times.start, event.context.timezone);
    if (date && times?.end) intent.end = localIso(date, times.end, event.context.timezone);
    if (!date || !times) intent.ambiguity = "missing_time";
    return intent;
  }

  private extractTitle(text: string): string | undefined {
    const titleMatch =
      /(?:今天|明天|后天).{0,16}?点(?:半)?(?:到|-|至).{1,12}?点(?:半)?的(.+?)(?:，|,|。|帮我|请|$)/.exec(text) ??
      /(?:建|创建|安排|预约).{0,8}(?:日程|会议|会).*?(?:主题|标题)(?:是|为)(.+?)(?:，|,|。|$)/.exec(text);
    const title = titleMatch?.[1]?.trim();
    return title ? title.replace(/^(一个|一次)/, "").trim() : undefined;
  }

  private extractAttendees(text: string): string[] {
    const attendeeText = /邀请(.+?)(?:参会|参加|加入|$)/.exec(text)?.[1];
    if (!attendeeText) return [];
    return attendeeText
      .split(/[、,，和及\s]+/u)
      .map((name) => name.trim())
      .filter(Boolean);
  }

  private extractTimes(text: string): { start: TimeOfDay; end: TimeOfDay } | undefined {
    const match = /(上午|下午|晚上|早上|中午)?\s*(\d{1,2})\s*点(半)?\s*(?:到|-|至)\s*(上午|下午|晚上|早上|中午)?\s*(\d{1,2})\s*点(半)?/.exec(text);
    if (!match) return undefined;
    const startPeriod = match[1];
    const endPeriod = match[4] ?? startPeriod;
    const startHour = Number(match[2]);
    const endHour = Number(match[5]);
    if (!Number.isInteger(startHour) || !Number.isInteger(endHour)) return undefined;
    return {
      start: { hour: normalizeHour(startHour, startPeriod), minute: match[3] ? 30 : 0 },
      end: { hour: normalizeHour(endHour, endPeriod), minute: match[6] ? 30 : 0 }
    };
  }
}

function normalizeHour(hour: number, period: string | undefined): number {
  if ((period === "下午" || period === "晚上") && hour < 12) return hour + 12;
  if (period === "中午" && hour < 11) return hour + 12;
  return hour;
}
