import { describe, expect, it } from "vitest";
import {
  AgendaBriefingAssistant,
  calculatePreviousWorkWeekWindow,
  calculateTodayWindow,
  classifyAgendaEvent,
  selectAgendaHighlights
} from "../src/agenda/assistant.js";
import { validateStructuredResponse } from "../src/contracts.js";
import type { LarkCalendarListRequest, LarkCalendarListResult, NormalizedAgendaEvent } from "../src/lark/adapters.js";
import { FakeCalendarAdapter, sampleEvent, sampleRecapEvents, sampleTodayEvents } from "./helpers.js";

describe("AgendaBriefingAssistant", () => {
  it("generates a normal briefing with list and summary actions", async () => {
    const calendar = new FakeCalendarAdapter(undefined, (request) => agendaResult(request));
    const response = await new AgendaBriefingAssistant({ calendarAdapter: calendar }).handle({
      ...sampleEvent,
      type: "head_touch",
      timestamp: "2026-06-06T08:00:00+08:00"
    });

    expect(validateStructuredResponse(response).ok).toBe(true);
    expect(response.follow_up.expected).toBe(false);
    expect(response.actions).toHaveLength(3);
    expect(response.actions[0]).toMatchObject({ type: "lark.calendar.list", status: "success" });
    expect(response.actions[1]).toMatchObject({ type: "lark.calendar.list", status: "success" });
    expect(response.actions[2]).toMatchObject({ type: "agenda.summary.generate", status: "success" });
    expect(response.context_patch).toMatchObject({
      briefing_date: "2026-06-06",
      today_event_count: 3
    });
    expect(response.context_patch.highlight_events).toEqual(["today_customer", "today_internal", "today_focus"]);
    expect(response.context_patch.category_counts).toMatchObject({
      internal_meeting: 1,
      customer_reception: 1,
      outdoor_activity: 1
    });
  });

  it("handles an empty today agenda", async () => {
    const calendar = new FakeCalendarAdapter(undefined, (request) =>
      agendaResult(request, { today: [], recap: sampleRecapEvents })
    );
    const response = await new AgendaBriefingAssistant({ calendarAdapter: calendar }).handle({
      ...sampleEvent,
      type: "daily_briefing_triggered",
      timestamp: "2026-06-06T08:00:00+08:00"
    });
    expect(response.speech).toContain("今天没有日历事项");
    expect(response.context_patch.today_event_count).toBe(0);
    expect(response.follow_up.expected).toBe(false);
  });

  it("handles an empty recap window", async () => {
    const calendar = new FakeCalendarAdapter(undefined, (request) =>
      agendaResult(request, { today: sampleTodayEvents, recap: [] })
    );
    const response = await new AgendaBriefingAssistant({ calendarAdapter: calendar }).handle({
      ...sampleEvent,
      type: "head_touch",
      timestamp: "2026-06-06T08:00:00+08:00"
    });
    expect(response.speech).toContain("最近工作周没有可回顾的日历事项");
    expect(response.context_patch.category_counts).toMatchObject({
      internal_meeting: 0,
      customer_reception: 0,
      outdoor_activity: 0,
      deep_work: 0,
      uncategorized: 0
    });
  });

  it("returns a degraded response and failed list action when a query fails", async () => {
    const calendar = new FakeCalendarAdapter(undefined, (request) => {
      if (request.end <= "2026-05-31T00:00:00.000Z") {
        return { ok: false, code: "LARK_CALENDAR_LIST_FAILED", message: "permission denied" };
      }
      return { ok: true, calendarId: "primary", events: sampleTodayEvents };
    });
    const response = await new AgendaBriefingAssistant({ calendarAdapter: calendar }).handle({
      ...sampleEvent,
      type: "head_touch",
      timestamp: "2026-06-06T08:00:00+08:00"
    });
    expect(response.speech).toContain("日历查询有部分失败");
    expect(response.actions.some((action) => action.status === "failed")).toBe(true);
    expect(response.actions.every((action) => action.type !== "lark.calendar.create")).toBe(true);
  });

  it("selects ordered highlights after priority scoring", () => {
    const highlights = selectAgendaHighlights(
      sampleTodayEvents.map((agendaEvent) => classifyAgendaEvent(agendaEvent)),
      2
    );
    expect(highlights.map((agendaEvent) => agendaEvent.id)).toEqual(["today_customer", "today_internal"]);
  });
});

describe("agenda classifier", () => {
  const cases: Array<[string, NormalizedAgendaEvent, string]> = [
    ["internal_meeting", event("内部例会"), "internal_meeting"],
    ["customer_reception", event("客户来访"), "customer_reception"],
    ["outdoor_activity", event("外出拜访", "客户园区"), "outdoor_activity"],
    ["deep_work", event("专注写作方案"), "deep_work"],
    ["uncategorized", event("阅读资料"), "uncategorized"]
  ];

  it.each(cases)("classifies %s", (_name, agendaEvent, category) => {
    expect(classifyAgendaEvent(agendaEvent).category).toBe(category);
  });
});

describe("agenda window calculation", () => {
  it("calculates local-day windows from timestamp and timezone", () => {
    expect(calculateTodayWindow("2026-06-06T08:00:00+08:00", "Asia/Shanghai")).toEqual({
      start: "2026-06-05T16:00:00.000Z",
      end: "2026-06-06T16:00:00.000Z",
      localDate: "2026-06-06"
    });
  });

  it("calculates the previous Monday-through-Friday recap window", () => {
    expect(calculatePreviousWorkWeekWindow("2026-06-06T08:00:00+08:00", "Asia/Shanghai")).toEqual({
      start: "2026-05-24T16:00:00.000Z",
      end: "2026-05-29T16:00:00.000Z",
      localDate: "2026-05-25/2026-05-29"
    });
  });
});

function agendaResult(
  request: LarkCalendarListRequest,
  fixtures: { today: NormalizedAgendaEvent[]; recap: NormalizedAgendaEvent[] } = {
    today: sampleTodayEvents,
    recap: sampleRecapEvents
  }
): LarkCalendarListResult {
  return {
    ok: true,
    calendarId: "primary",
    events: request.end <= "2026-05-31T00:00:00.000Z" ? fixtures.recap : fixtures.today
  };
}

function event(title: string, location?: string): NormalizedAgendaEvent {
  return {
    id: title,
    title,
    start: "2026-06-06T10:00:00+08:00",
    end: "2026-06-06T11:00:00+08:00",
    location
  };
}
