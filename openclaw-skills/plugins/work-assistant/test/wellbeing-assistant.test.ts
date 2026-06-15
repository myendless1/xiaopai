import { describe, expect, it } from "vitest";
import { validateStructuredResponse } from "../src/contracts.js";
import type { LarkCalendarListRequest, LarkCalendarListResult, NormalizedAgendaEvent } from "../src/lark/adapters.js";
import { WellbeingCompanionAssistant } from "../src/wellbeing/assistant.js";
import { FakeCalendarAdapter, sampleEvent, sampleTodayEvents } from "./helpers.js";

describe("WellbeingCompanionAssistant", () => {
  it("generates a valid sedentary nudge with action records and context patch", async () => {
    const calendar = new FakeCalendarAdapter(undefined, emptyListResult);
    const response = await new WellbeingCompanionAssistant({ calendarAdapter: calendar }).handle(sedentaryEvent());

    expect(validateStructuredResponse(response).ok).toBe(true);
    expect(response.speech).toContain("要不要听个短笑话");
    expect(response.follow_up).toMatchObject({
      expected: true,
      reason: "wellbeing_companion_offer"
    });
    expect(response.actions).toHaveLength(2);
    expect(response.actions[0]).toMatchObject({ type: "lark.calendar.list", status: "success" });
    expect(response.actions[1]).toMatchObject({
      type: "wellbeing.sedentary.evaluate",
      status: "success",
      details: {
        decision: "allowed",
        duration_minutes: 35,
        confidence: 0.91
      }
    });
    expect(response.context_patch).toMatchObject({
      wellbeing_last_nudge_at: "2026-06-06T13:40:00+08:00",
      wellbeing_last_decision: "allowed",
      wellbeing_follow_up_offered: true
    });
    expect(calendar.listCalls).toHaveLength(1);
  });

  it("skips invalid sedentary payloads without nudge speech", async () => {
    const calendar = new FakeCalendarAdapter(undefined, emptyListResult);
    const response = await new WellbeingCompanionAssistant({ calendarAdapter: calendar }).handle(
      sedentaryEvent({
        payload: {
          duration_minutes: 35
        }
      })
    );

    expect(response.speech).toBe("");
    expect(response.follow_up.expected).toBe(false);
    expect(response.actions).toHaveLength(1);
    expect(response.actions[0]).toMatchObject({
      type: "wellbeing.sedentary.evaluate",
      status: "skipped",
      details: {
        decision: "invalid_payload",
        reason: "invalid_payload"
      }
    });
    expect(calendar.listCalls).toHaveLength(0);
  });

  it("skips low confidence and short duration events through configured thresholds", async () => {
    const assistant = new WellbeingCompanionAssistant({
      calendarAdapter: new FakeCalendarAdapter(undefined, emptyListResult),
      minimumConfidence: 0.9,
      minimumSedentaryDurationMinutes: 30
    });

    const lowConfidence = await assistant.handle(
      sedentaryEvent({
        event_id: "evt-low-confidence",
        payload: {
          duration_minutes: 35,
          confidence: 0.7
        }
      })
    );
    const shortDuration = await assistant.handle(
      sedentaryEvent({
        event_id: "evt-short-duration",
        payload: {
          duration_minutes: 15,
          confidence: 0.95
        }
      })
    );

    expect(lowConfidence.actions[0]).toMatchObject({
      status: "skipped",
      details: {
        decision: "low_confidence",
        minimum_confidence: 0.9
      }
    });
    expect(shortDuration.actions[0]).toMatchObject({
      status: "skipped",
      details: {
        decision: "insufficient_duration",
        minimum_duration_minutes: 30
      }
    });
    expect(lowConfidence.speech).toBe("");
    expect(shortDuration.speech).toBe("");
  });

  it("suppresses duplicate nudges inside the cooldown window from supplied context", async () => {
    const response = await new WellbeingCompanionAssistant({
      calendarAdapter: new FakeCalendarAdapter(undefined, emptyListResult),
      cooldownMinutes: 30
    }).handle(
      sedentaryEvent({
        context: {
          timezone: "Asia/Shanghai",
          wellbeing_last_nudge_at: "2026-06-06T13:25:00+08:00"
        }
      })
    );

    expect(response.speech).toBe("");
    expect(response.follow_up.expected).toBe(false);
    expect(response.actions[0]).toMatchObject({
      status: "skipped",
      details: {
        decision: "cooldown",
        reason: "cooldown",
        minutes_since_last_nudge: 15
      }
    });
  });

  it("suppresses audible nudges during current calendar events", async () => {
    const calendar = new FakeCalendarAdapter(undefined, sampleListResult);
    const response = await new WellbeingCompanionAssistant({ calendarAdapter: calendar }).handle(
      sedentaryEvent({
        event_id: "evt-meeting-overlap",
        timestamp: "2026-06-06T14:10:00+08:00"
      })
    );

    expect(response.speech).toBe("");
    expect(response.actions[0]).toMatchObject({ type: "lark.calendar.list", status: "success" });
    expect(response.actions[1]).toMatchObject({
      type: "wellbeing.sedentary.evaluate",
      status: "skipped",
      details: {
        decision: "meeting_overlap",
        reason: "meeting_overlap"
      }
    });
    expect(calendar.listCalls[0]).toMatchObject({
      start: "2026-06-06T06:05:00.000Z",
      end: "2026-06-06T06:40:00.000Z"
    });
  });

  it("includes at most one upcoming event reminder in allowed nudges", async () => {
    const response = await new WellbeingCompanionAssistant({
      calendarAdapter: new FakeCalendarAdapter(undefined, sampleListResult)
    }).handle(sedentaryEvent());

    expect(response.speech).toContain("项目内部同步");
    expect(response.speech).not.toContain("专注写作方案");
    expect(response.context_patch.wellbeing_nearby_event).toMatchObject({
      event_id: "today_internal",
      title: "项目内部同步"
    });
  });

  it("degrades gracefully when calendar lookup fails", async () => {
    const response = await new WellbeingCompanionAssistant({
      calendarAdapter: new FakeCalendarAdapter(undefined, {
        ok: false,
        code: "LARK_CALENDAR_LIST_FAILED",
        message: "permission denied"
      })
    }).handle(sedentaryEvent({ event_id: "evt-calendar-failure" }));

    expect(response.speech).not.toBe("");
    expect(response.actions).toHaveLength(2);
    expect(response.actions[0]).toMatchObject({
      type: "lark.calendar.list",
      status: "failed",
      error: {
        code: "LARK_CALENDAR_LIST_FAILED"
      }
    });
    expect(response.actions[1]).toMatchObject({
      type: "wellbeing.sedentary.evaluate",
      status: "success",
      details: {
        decision: "allowed",
        calendar_degraded: true
      }
    });
    expect(response.actions.every((action) => action.type !== "lark.calendar.create")).toBe(true);
  });

  it("generates bounded follow-up companionship content and action records", async () => {
    const response = await new WellbeingCompanionAssistant({
      calendarAdapter: new FakeCalendarAdapter(undefined, emptyListResult)
    }).handle({
      ...sampleEvent,
      event_id: "evt-follow-up-relax",
      type: "wellbeing_companion_requested",
      payload: {
        content_type: "relaxation"
      }
    });

    expect(response.speech.length).toBeLessThanOrEqual(80);
    expect(response.presentation.emotion).toBe("positive");
    expect(response.follow_up.expected).toBe(false);
    expect(response.actions).toEqual([
      {
        type: "wellbeing.companion.generate",
        status: "success",
        details: {
          content_type: "relaxation",
          bounded: true
        }
      }
    ]);
  });
});

function sedentaryEvent(overrides: Record<string, unknown> = {}) {
  return {
    ...sampleEvent,
    event_id: "evt-sedentary",
    type: "sedentary_detected",
    timestamp: "2026-06-06T13:40:00+08:00",
    payload: {
      duration_minutes: 35,
      confidence: 0.91,
      source: "robot_vision"
    },
    ...overrides
  };
}

function emptyListResult(request: LarkCalendarListRequest): LarkCalendarListResult {
  return {
    ok: true,
    calendarId: request.calendarId ?? "primary",
    events: []
  };
}

function sampleListResult(request: LarkCalendarListRequest): LarkCalendarListResult {
  return {
    ok: true,
    calendarId: request.calendarId ?? "primary",
    events: sampleTodayEvents as NormalizedAgendaEvent[]
  };
}
