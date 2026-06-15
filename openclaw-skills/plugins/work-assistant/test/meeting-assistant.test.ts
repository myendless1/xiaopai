import { describe, expect, it } from "vitest";
import { MeetingReminderAssistant } from "../src/meeting/assistant.js";
import { FakeIMAdapter, sampleEvent } from "./helpers.js";

describe("meeting reminder assistant", () => {
  it("generates reminder speech, action shape, and current focus patches", async () => {
    const assistant = new MeetingReminderAssistant({ imAdapter: new FakeIMAdapter() });
    const response = await assistant.handleReminder({
      ...sampleEvent,
      event_id: "evt-reminder",
      type: "meeting_starting_soon",
      timestamp: "2026-06-06T09:20:00+08:00",
      payload: {
        trigger: {
          rule_id: "meeting_starting_soon",
          scheduled_for: "2026-06-06T01:20:00.000Z",
          fired_at: "2026-06-06T01:20:00.000Z",
          source: "proactive_calendar_scheduler",
          trigger_key: "trigger_abc"
        },
        calendar_event: {
          id: "calendar_1",
          title: "项目同步",
          start: "2026-06-06T09:30:00+08:00",
          end: "2026-06-06T10:00:00+08:00",
          calendar_id: "primary",
          location: "线上会议",
          notification_target: {
            attendee_user_ids: ["ou_1", "ou_2"]
          }
        }
      }
    });

    expect(response.speech).toContain("项目同步");
    expect(response.speech).toContain("线上会议");
    expect(response.actions[0]).toMatchObject({
      type: "meeting.reminder.generate",
      status: "success",
      details: {
        event_id: "calendar_1",
        calendar_id: "primary",
        trigger_key: "trigger_abc"
      }
    });
    expect(response.context_patch.current_focus).toMatchObject({
      type: "calendar_event",
      event_id: "calendar_1",
      title: "项目同步",
      start_time: "2026-06-06T09:30:00+08:00",
      end_time: "2026-06-06T10:00:00+08:00",
      notification_target: {
        attendee_user_ids: ["ou_1", "ou_2"]
      }
    });
  });

  it("returns failed reminder actions without side effects for malformed scheduler events", async () => {
    const im = new FakeIMAdapter();
    const assistant = new MeetingReminderAssistant({ imAdapter: im });
    const response = await assistant.handleReminder({
      ...sampleEvent,
      event_id: "evt-reminder-malformed",
      type: "meeting_starting_soon",
      payload: {
        trigger: {
          rule_id: "meeting_starting_soon"
        }
      }
    });

    expect(response.follow_up.expected).toBe(false);
    expect(response.actions).toEqual([
      {
        type: "meeting.reminder.generate",
        status: "failed",
        error: {
          code: "MALFORMED_MEETING_REMINDER_EVENT",
          message: "payload.calendar_event must include id, title, start, and end."
        }
      }
    ]);
    expect(im.calls).toHaveLength(0);
  });
});
