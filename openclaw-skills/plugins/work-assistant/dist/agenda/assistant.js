import { followUpResponse } from "../contracts.js";
const DEFAULT_MAX_HIGHLIGHTS = 3;
const DEFAULT_RULES = {
    customer_reception: ["客户", "customer", "client", "来访", "接待", "拜访", "external"],
    outdoor_activity: ["外出", "出差", "拜访", "园区", "site", "field", "户外", "活动"],
    internal_meeting: ["内部", "例会", "周会", "同步", "评审", "站会", "sync", "review"],
    deep_work: ["专注", "deep work", "focus", "写作", "方案", "研发", "coding", "blocked"],
    uncategorized: []
};
export class AgendaBriefingAssistant {
    calendarAdapter;
    calendarId;
    maxHighlights;
    rules;
    constructor(options) {
        this.calendarAdapter = options.calendarAdapter;
        this.calendarId = options.calendarId;
        this.maxHighlights = options.maxHighlights ?? DEFAULT_MAX_HIGHLIGHTS;
        this.rules = options.rules ?? DEFAULT_RULES;
    }
    async handle(event) {
        let todayWindow;
        let recapWindow;
        try {
            todayWindow = calculateTodayWindow(event.timestamp, event.context.timezone);
            recapWindow = calculatePreviousWorkWeekWindow(event.timestamp, event.context.timezone);
        }
        catch (error) {
            return followUpResponse("请提供有效的事件时间和时区后再生成日程简报。", "invalid_briefing_context", "日程简报需要有效的时间和时区。");
        }
        const todayResult = await this.listWindow("today", todayWindow);
        const recapResult = await this.listWindow("recap", recapWindow);
        const actions = [todayResult.action, recapResult.action];
        const todayEvents = "events" in todayResult ? todayResult.events : [];
        const recapEvents = "events" in recapResult ? recapResult.events : [];
        const classifiedToday = todayEvents.map((agendaEvent) => classifyAgendaEvent(agendaEvent, this.rules));
        const classifiedRecap = recapEvents.map((agendaEvent) => classifyAgendaEvent(agendaEvent, this.rules));
        const categoryCounts = countByCategory(classifiedRecap);
        const highlights = selectAgendaHighlights(classifiedToday, this.maxHighlights);
        const degraded = !("events" in todayResult) || !("events" in recapResult);
        actions.push({
            type: "agenda.summary.generate",
            status: "success",
            details: {
                briefing_date: todayWindow.localDate,
                today_event_count: todayEvents.length,
                recap_event_count: recapEvents.length,
                highlight_count: highlights.length,
                degraded
            }
        });
        return {
            speech: buildSpeech({
                todayEvents: classifiedToday,
                recapEvents: classifiedRecap,
                highlights,
                categoryCounts,
                degraded
            }),
            presentation: {
                emotion: degraded ? "concerned" : todayEvents.length === 0 ? "calm" : "focused",
                motion: todayEvents.length === 0 ? "idle" : "look_at_user",
                light: degraded ? "amber" : "soft_blink"
            },
            actions,
            follow_up: {
                expected: false
            },
            context_patch: {
                briefing_date: todayWindow.localDate,
                today_event_count: todayEvents.length,
                highlight_events: highlights.map((agendaEvent) => agendaEvent.id || agendaEvent.title),
                category_counts: categoryCounts
            }
        };
    }
    async listWindow(windowType, window) {
        const request = {
            start: window.start,
            end: window.end
        };
        if (this.calendarId)
            request.calendarId = this.calendarId;
        const result = await this.calendarAdapter.listEvents(request);
        if (!result.ok) {
            return {
                action: {
                    type: "lark.calendar.list",
                    status: "failed",
                    error: {
                        code: result.code,
                        message: result.message
                    },
                    details: {
                        window: windowType,
                        start: window.start,
                        end: window.end
                    }
                }
            };
        }
        return {
            events: result.events,
            action: {
                type: "lark.calendar.list",
                status: "success",
                details: {
                    window: windowType,
                    start: window.start,
                    end: window.end,
                    calendar_id: result.calendarId ?? this.calendarId ?? "primary",
                    event_count: result.events.length
                }
            }
        };
    }
}
export function calculateTodayWindow(timestamp, timezone) {
    const parts = getLocalDateParts(timestamp, timezone);
    const start = zonedTimeToUtcIso(parts.year, parts.month, parts.day, 0, 0, 0, 0, timezone);
    const nextDay = addUtcDays(Date.UTC(parts.year, parts.month - 1, parts.day), 1);
    const nextParts = {
        year: nextDay.getUTCFullYear(),
        month: nextDay.getUTCMonth() + 1,
        day: nextDay.getUTCDate()
    };
    const end = zonedTimeToUtcIso(nextParts.year, nextParts.month, nextParts.day, 0, 0, 0, 0, timezone);
    return {
        start,
        end,
        localDate: formatLocalDate(parts)
    };
}
export function calculatePreviousWorkWeekWindow(timestamp, timezone) {
    const parts = getLocalDateParts(timestamp, timezone);
    const localNoon = Date.UTC(parts.year, parts.month - 1, parts.day, 12, 0, 0, 0);
    const dayOfWeek = new Date(localNoon).getUTCDay();
    const daysSinceMonday = (dayOfWeek + 6) % 7;
    const previousMonday = addUtcDays(localNoon, -(daysSinceMonday + 7));
    const followingSaturday = addUtcDays(previousMonday, 5);
    const start = zonedTimeToUtcIso(previousMonday.getUTCFullYear(), previousMonday.getUTCMonth() + 1, previousMonday.getUTCDate(), 0, 0, 0, 0, timezone);
    const end = zonedTimeToUtcIso(followingSaturday.getUTCFullYear(), followingSaturday.getUTCMonth() + 1, followingSaturday.getUTCDate(), 0, 0, 0, 0, timezone);
    return {
        start,
        end,
        localDate: `${formatLocalDate({
            year: previousMonday.getUTCFullYear(),
            month: previousMonday.getUTCMonth() + 1,
            day: previousMonday.getUTCDate()
        })}/${formatLocalDate({
            year: addUtcDays(previousMonday, 4).getUTCFullYear(),
            month: addUtcDays(previousMonday, 4).getUTCMonth() + 1,
            day: addUtcDays(previousMonday, 4).getUTCDate()
        })}`
    };
}
export function classifyAgendaEvent(agendaEvent, rules = DEFAULT_RULES) {
    const haystack = [agendaEvent.title, agendaEvent.location, agendaEvent.description]
        .filter((value) => typeof value === "string")
        .join(" ")
        .toLocaleLowerCase();
    const priority = [
        "outdoor_activity",
        "customer_reception",
        "internal_meeting",
        "deep_work"
    ];
    for (const category of priority) {
        const matched = rules[category].find((keyword) => haystack.includes(keyword.toLocaleLowerCase()));
        if (matched) {
            return {
                ...agendaEvent,
                category,
                classificationReason: `keyword:${matched}`
            };
        }
    }
    return {
        ...agendaEvent,
        category: "uncategorized",
        classificationReason: "fallback"
    };
}
export function countByCategory(events) {
    const counts = {
        internal_meeting: 0,
        customer_reception: 0,
        outdoor_activity: 0,
        deep_work: 0,
        uncategorized: 0
    };
    for (const agendaEvent of events)
        counts[agendaEvent.category] += 1;
    return counts;
}
export function selectAgendaHighlights(events, limit = DEFAULT_MAX_HIGHLIGHTS) {
    const sorted = [...events].sort((left, right) => {
        const leftScore = highlightScore(left);
        const rightScore = highlightScore(right);
        if (leftScore !== rightScore)
            return rightScore - leftScore;
        return Date.parse(left.start) - Date.parse(right.start);
    });
    return sorted.slice(0, limit).sort((left, right) => Date.parse(left.start) - Date.parse(right.start));
}
function highlightScore(event) {
    let score = Number.isFinite(Date.parse(event.start)) ? 1 : 0;
    if (event.location)
        score += 2;
    if (event.attendeeCount && event.attendeeCount >= 3)
        score += 1;
    if (event.category === "customer_reception")
        score += 5;
    if (event.category === "outdoor_activity")
        score += 4;
    if (event.category === "internal_meeting")
        score += 2;
    if (/准备|prep|材料|review|评审/i.test(`${event.title} ${event.description ?? ""}`))
        score += 2;
    return score;
}
function buildSpeech(input) {
    const parts = [];
    if (input.degraded)
        parts.push("日历查询有部分失败，我先根据能读取到的信息给你简报。");
    if (input.todayEvents.length === 0) {
        parts.push("今天没有日历事项。");
    }
    else {
        parts.push(`今天共有 ${input.todayEvents.length} 个日历事项。`);
        if (input.highlights.length > 0) {
            parts.push(`重点关注：${input.highlights.map(formatHighlight).join("；")}。`);
        }
    }
    if (input.recapEvents.length === 0) {
        parts.push("最近工作周没有可回顾的日历事项。");
    }
    else {
        const recapBits = [
            ["内部会议", input.categoryCounts.internal_meeting],
            ["客户接待", input.categoryCounts.customer_reception],
            ["外出活动", input.categoryCounts.outdoor_activity],
            ["专注工作", input.categoryCounts.deep_work]
        ]
            .filter(([, count]) => Number(count) > 0)
            .map(([label, count]) => `${label} ${count} 个`);
        parts.push(`上个工作周共有 ${input.recapEvents.length} 个事项${recapBits.length > 0 ? `，其中${recapBits.join("、")}` : ""}。`);
    }
    return parts.join("");
}
function formatHighlight(event) {
    const time = formatTime(event.start);
    const location = event.location ? `，地点 ${event.location}` : "";
    return `${time}${event.title}${location}`;
}
function formatTime(value) {
    const date = new Date(value);
    if (!Number.isFinite(date.getTime()))
        return "";
    return `${String(date.getUTCHours()).padStart(2, "0")}:${String(date.getUTCMinutes()).padStart(2, "0")} `;
}
function getLocalDateParts(timestamp, timezone) {
    const date = new Date(timestamp);
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
function zonedTimeToUtcIso(year, month, day, hour, minute, second, millisecond, timezone) {
    const localAsUtc = Date.UTC(year, month - 1, day, hour, minute, second, millisecond);
    let utc = localAsUtc - getTimezoneOffsetMs(localAsUtc, timezone);
    utc = localAsUtc - getTimezoneOffsetMs(utc, timezone);
    return new Date(utc).toISOString();
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
function addUtcDays(value, days) {
    const ms = value instanceof Date ? value.getTime() : value;
    return new Date(ms + days * 24 * 60 * 60 * 1000);
}
function formatLocalDate(parts) {
    return `${parts.year}-${String(parts.month).padStart(2, "0")}-${String(parts.day).padStart(2, "0")}`;
}
//# sourceMappingURL=assistant.js.map