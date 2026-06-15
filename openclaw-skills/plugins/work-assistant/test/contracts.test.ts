import { describe, expect, it } from "vitest";
import { validateInputEvent, validateStructuredAssistantIntent, validateStructuredResponse } from "../src/contracts.js";
import { sampleEvent } from "./helpers.js";

describe("assistant contracts", () => {
  it("validates a normalized user utterance event", () => {
    const result = validateInputEvent(sampleEvent);
    expect(result.ok).toBe(true);
    if (result.ok) {
      expect(result.value.payload.text).toContain("项目会");
    }
  });

  it("validates agenda briefing trigger events with the existing input envelope", () => {
    expect(validateInputEvent({ ...sampleEvent, type: "head_touch", payload: {} }).ok).toBe(true);
    expect(validateInputEvent({ ...sampleEvent, type: "daily_briefing_triggered", payload: {} }).ok).toBe(true);
  });

  it("validates wellbeing events with the existing input envelope", () => {
    expect(
      validateInputEvent({
        ...sampleEvent,
        type: "sedentary_detected",
        payload: {
          duration_minutes: 35,
          confidence: 0.91
        }
      }).ok
    ).toBe(true);
    expect(
      validateInputEvent({
        ...sampleEvent,
        type: "wellbeing_companion_requested",
        payload: {
          content_type: "joke"
        }
      }).ok
    ).toBe(true);
  });

  it("validates scheduler-produced proactive event metadata", () => {
    const result = validateInputEvent({
      ...sampleEvent,
      event_id: "proactive_abc",
      type: "meeting_starting_soon",
      timestamp: "2026-06-06T09:20:00+08:00",
      payload: {
        trigger: {
          rule_id: "meeting_starting_soon",
          scheduled_for: "2026-06-06T09:20:00.000Z",
          fired_at: "2026-06-06T09:20:00.000Z",
          source: "proactive_calendar_scheduler",
          trigger_key: "trigger_abc",
          calendar_id: "primary",
          source_event_id: "calendar_1"
        },
        calendar_event: {
          id: "calendar_1",
          title: "客户来访接待",
          start: "2026-06-06T09:30:00+08:00",
          end: "2026-06-06T10:30:00+08:00",
          calendar_id: "primary",
          location: "上海办公室"
        }
      }
    });
    expect(result.ok).toBe(true);
    if (result.ok) {
      expect(result.value.payload.trigger).toMatchObject({
        rule_id: "meeting_starting_soon",
        source: "proactive_calendar_scheduler"
      });
      expect(result.value.payload.calendar_event).toMatchObject({
        id: "calendar_1",
        title: "客户来访接待"
      });
    }
  });

  it("validates scheduler-produced travel events through the shared input envelope", () => {
    for (const type of ["outdoor_event_detected", "business_trip_tomorrow_detected"]) {
      const result = validateInputEvent({
        ...sampleEvent,
        event_id: `proactive_${type}`,
        type,
        timestamp: "2026-06-06T17:00:00+08:00",
        payload: {
          trigger: {
            rule_id: type === "outdoor_event_detected" ? "outdoor_event" : "business_trip_tomorrow",
            scheduled_for: "2026-06-06T09:00:00.000Z",
            fired_at: "2026-06-06T09:00:00.000Z",
            source: "proactive_calendar_scheduler",
            trigger_key: `trigger_${type}`,
            calendar_id: "primary",
            source_event_id: "calendar_travel"
          },
          calendar_event: {
            id: "calendar_travel",
            title: type === "outdoor_event_detected" ? "外出客户园区拜访" : "北京出差",
            start: type === "outdoor_event_detected" ? "2026-06-06T18:00:00+08:00" : "2026-06-07T09:00:00+08:00",
            end: type === "outdoor_event_detected" ? "2026-06-06T19:30:00+08:00" : "2026-06-07T20:00:00+08:00",
            calendar_id: "primary",
            location: type === "outdoor_event_detected" ? "客户园区" : "北京"
          }
        }
      });
      expect(result.ok).toBe(true);
    }
  });

  it("rejects an invalid event shape", () => {
    const result = validateInputEvent({ ...sampleEvent, timestamp: "not-a-date" });
    expect(result.ok).toBe(false);
  });

  it("validates the structured response shape", () => {
    const result = validateStructuredResponse({
      speech: "ok",
      presentation: {},
      actions: [],
      follow_up: { expected: false },
      context_patch: {}
    });
    expect(result.ok).toBe(true);
  });

  it("validates wellbeing action records in structured responses", () => {
    const result = validateStructuredResponse({
      speech: "",
      presentation: { emotion: "quiet" },
      actions: [
        {
          type: "wellbeing.sedentary.evaluate",
          status: "skipped",
          details: {
            decision: "low_confidence",
            reason: "low_confidence"
          }
        }
      ],
      follow_up: { expected: false },
      context_patch: {
        wellbeing_last_decision: "low_confidence"
      }
    });
    expect(result.ok).toBe(true);
  });

  it("validates supported structured calendar intents", () => {
    const result = validateStructuredAssistantIntent({
      type: "calendar.create",
      version: "1",
      title: "OpenClaw 测试",
      start: "2026-06-06T10:00:00+08:00",
      end: "2026-06-06T10:30:00+08:00",
      attendees: [{ name: "Gargantua" }, { id: "ou_direct" }]
    });

    expect(result.ok).toBe(true);
    if (result.ok) {
      expect(result.value.attendees).toEqual([{ name: "Gargantua" }, { id: "ou_direct" }]);
    }
  });

  it("validates supported structured meeting notification intents", () => {
    const result = validateStructuredAssistantIntent({
      type: "meeting.notify_late",
      version: "1",
      delay_minutes: 5,
      message: "我会晚五分钟到，请大家稍等一下"
    });

    expect(result.ok).toBe(true);
    if (result.ok) {
      expect(result.value).toEqual({
        type: "meeting.notify_late",
        version: "1",
        delay_minutes: 5,
        message: "我会晚五分钟到，请大家稍等一下"
      });
    }
  });

  it("returns specific structured intent validation reasons", () => {
    expect(validateStructuredAssistantIntent("bad")).toMatchObject({
      ok: false,
      reason: "malformed_structured_intent"
    });
    expect(validateStructuredAssistantIntent({ type: "task.create", version: "1" })).toMatchObject({
      ok: false,
      reason: "unsupported_intent_type"
    });
    expect(validateStructuredAssistantIntent({ type: "calendar.create", version: "2" })).toMatchObject({
      ok: false,
      reason: "unsupported_intent_version"
    });
    expect(validateStructuredAssistantIntent({ type: "meeting.notify_late", version: "2" })).toMatchObject({
      ok: false,
      reason: "unsupported_intent_version"
    });
    expect(validateStructuredAssistantIntent({ type: "calendar.create", version: "1" })).toMatchObject({
      ok: false,
      reason: "missing_required_structured_fields"
    });
    expect(
      validateStructuredAssistantIntent({
        type: "calendar.create",
        version: "1",
        title: "OpenClaw 测试",
        start: "2026-06-06T10:30:00+08:00",
        end: "2026-06-06T10:00:00+08:00",
        attendees: [{ name: "Gargantua" }]
      })
    ).toMatchObject({
      ok: false,
      reason: "invalid_time_range"
    });
    expect(
      validateStructuredAssistantIntent({
        type: "calendar.create",
        version: "1",
        title: "OpenClaw 测试",
        start: "2026-06-06T10:00:00+08:00",
        end: "2026-06-06T10:30:00+08:00",
        attendees: [{ id: "bad_direct" }]
      })
    ).toMatchObject({
      ok: false,
      reason: "invalid_attendee_reference"
    });
  });
});
