import { describe, expect, it } from "vitest";
import { FakeCalendarAdapter, FakeContactAdapter, FakeIMAdapter, createTestHandler, sampleEvent } from "./helpers.js";

describe("work assistant handler", () => {
  it("returns a successful structured calendar response", async () => {
    const { handler, calendar } = createTestHandler();
    const response = await handler.handleEvent(sampleEvent);
    expect(response.follow_up.expected).toBe(false);
    expect(response.actions).toHaveLength(1);
    expect(response.actions[0]?.type).toBe("lark.calendar.create");
    expect(response.actions[0]?.status).toBe("success");
    expect(response.context_patch.last_created_calendar_event_id).toBe("evt_lark_1");
    expect(calendar.calls).toHaveLength(1);
    expect(calendar.calls[0]?.attendeeIds).toEqual(["ou_zhangsan", "ou_lisi"]);
  });

  it("returns a follow-up and skips side effects for missing time", async () => {
    const { handler, calendar } = createTestHandler();
    const response = await handler.handleEvent({
      ...sampleEvent,
      event_id: "evt-missing-time",
      payload: {
        text: "帮我建一个飞书日程，标题是项目会，邀请张三、李四参会。"
      }
    });
    expect(response.follow_up.expected).toBe(true);
    expect(response.follow_up.reason).toBe("missing_time");
    expect(calendar.calls).toHaveLength(0);
  });

  it("returns a follow-up and skips side effects for invalid time ranges", async () => {
    const { handler, calendar } = createTestHandler();
    const response = await handler.handleEvent({
      ...sampleEvent,
      event_id: "evt-invalid-time",
      payload: {
        text: "明天上午11点到10点的项目会，帮我建一个飞书日程，邀请张三、李四参会。"
      }
    });
    expect(response.follow_up.expected).toBe(true);
    expect(response.follow_up.reason).toBe("invalid_time_range");
    expect(calendar.calls).toHaveLength(0);
  });

  it("returns a follow-up and skips side effects for ambiguous attendees", async () => {
    const calendar = new FakeCalendarAdapter();
    const { handler } = createTestHandler({
      calendar,
      contact: new FakeContactAdapter({
        张三: {
          status: "ambiguous",
          candidates: [
            { id: "ou_1", name: "张三", department: "研发" },
            { id: "ou_2", name: "张三", department: "销售" }
          ]
        },
        李四: {
          status: "unique",
          person: { id: "ou_lisi", name: "李四" }
        }
      })
    });
    const response = await handler.handleEvent({ ...sampleEvent, event_id: "evt-ambiguous" });
    expect(response.follow_up.expected).toBe(true);
    expect(response.follow_up.reason).toBe("ambiguous_attendee");
    expect(calendar.calls).toHaveLength(0);
  });

  it("routes head_touch events to agenda briefing without calendar creation", async () => {
    const { handler, calendar } = createTestHandler();
    const response = await handler.handleEvent({ ...sampleEvent, event_id: "evt-head-touch", type: "head_touch" });
    expect(response.follow_up.expected).toBe(false);
    expect(response.actions.map((action) => action.type)).toEqual([
      "lark.calendar.list",
      "lark.calendar.list",
      "agenda.summary.generate"
    ]);
    expect(calendar.calls).toHaveLength(0);
    expect(calendar.listCalls).toHaveLength(2);
  });

  it("routes daily_briefing_triggered events to agenda briefing", async () => {
    const { handler, calendar } = createTestHandler();
    const response = await handler.handleEvent({
      ...sampleEvent,
      event_id: "evt-daily-briefing",
      type: "daily_briefing_triggered"
    });
    expect(response.follow_up.expected).toBe(false);
    expect(response.context_patch.today_event_count).toBe(3);
    expect(calendar.calls).toHaveLength(0);
    expect(calendar.listCalls).toHaveLength(2);
  });

  it("routes meeting_starting_soon events to the meeting reminder assistant", async () => {
    const { handler, calendar, im } = createTestHandler();
    const response = await handler.handleEvent(meetingStartingSoonEvent());

    expect(response.follow_up.expected).toBe(false);
    expect(response.speech).toContain("客户来访接待");
    expect(response.speech).toContain("上海办公室");
    expect(response.actions).toEqual([
      expect.objectContaining({
        type: "meeting.reminder.generate",
        status: "success"
      })
    ]);
    expect(response.context_patch.current_focus).toMatchObject({
      type: "calendar_event",
      event_id: "calendar_1",
      notification_target: {
        chat_id: "oc_meeting_chat"
      }
    });
    expect(calendar.calls).toHaveLength(0);
    expect(im.calls).toHaveLength(0);
  });

  it("routes outdoor travel events to the travel planner before calendar fallback", async () => {
    const { handler, calendar, route, weather, profile } = createTestHandler();
    const response = await handler.handleEvent(outdoorTravelEvent());

    expect(response.follow_up.expected).toBe(false);
    expect(response.speech).toContain("客户园区");
    expect(response.speech).toContain("17:03");
    expect(response.actions.map((action) => action.type)).toEqual([
      "user.profile.read",
      "route.estimate",
      "travel.plan.generate"
    ]);
    expect(route.calls).toHaveLength(1);
    expect(profile.calls).toHaveLength(1);
    expect(weather.calls).toHaveLength(0);
    expect(calendar.calls).toHaveLength(0);
  });

  it("routes business trip travel events to the travel planner before calendar fallback", async () => {
    const { handler, calendar, route, weather } = createTestHandler();
    const response = await handler.handleEvent(businessTripEvent());

    expect(response.follow_up.expected).toBe(false);
    expect(response.speech).toContain("北京");
    expect(response.speech).toContain("证件");
    expect(response.actions.map((action) => action.type)).toEqual(["weather.forecast", "travel.plan.generate"]);
    expect(weather.calls).toHaveLength(1);
    expect(route.calls).toHaveLength(0);
    expect(calendar.calls).toHaveLength(0);
  });

  it("routes structured late notification intents before calendar creation", async () => {
    const { handler, calendar, im } = createTestHandler();
    const response = await handler.handleEvent(lateNotificationEvent());

    expect(response.follow_up.expected).toBe(false);
    expect(response.actions[0]).toMatchObject({
      type: "lark.message.send",
      status: "success",
      resource_id: "om_lark_1",
      details: {
        target: { chat_id: "oc_meeting_chat" },
        meeting_event_id: "calendar_1",
        delay_minutes: 5,
        source: "structured"
      }
    });
    expect(im.calls).toHaveLength(1);
    expect(im.calls[0]).toMatchObject({
      chatId: "oc_meeting_chat",
      requesterId: "ou_requester",
      idempotencyKey: "evt-late-notify"
    });
    expect(im.calls[0]?.text).toContain("晚五分钟");
    expect(calendar.calls).toHaveLength(0);
  });

  it("returns a follow-up for unsupported meeting notification intent versions", async () => {
    const { handler, im } = createTestHandler();
    const response = await handler.handleEvent(
      lateNotificationEvent({
        event_id: "evt-late-version",
        payload: {
          structured_intent: {
            type: "meeting.notify_late",
            version: "2",
            delay_minutes: 5
          }
        }
      })
    );

    expect(response.follow_up.expected).toBe(true);
    expect(response.follow_up.reason).toBe("unsupported_intent_version");
    expect(im.calls).toHaveLength(0);
  });

  it("uses deterministic fallback parsing for clear late notification text", async () => {
    const { handler, im } = createTestHandler();
    const response = await handler.handleEvent(
      lateNotificationEvent({
        event_id: "evt-late-fallback",
        payload: {
          text: "我会晚到5分钟，帮我通知参会人"
        }
      })
    );

    expect(response.actions[0]).toMatchObject({
      type: "lark.message.send",
      status: "success",
      details: {
        delay_minutes: 5,
        source: "fallback"
      }
    });
    expect(im.calls[0]?.text).toContain("晚 5 分钟");
  });

  it("asks for meeting focus or recipients instead of sending ambiguous notifications", async () => {
    const { handler, im } = createTestHandler();
    const missingFocus = await handler.handleEvent({
      ...sampleEvent,
      event_id: "evt-late-missing-focus",
      payload: {
        text: "我会晚到5分钟，帮我通知参会人"
      }
    });
    const missingTarget = await handler.handleEvent(
      lateNotificationEvent({
        event_id: "evt-late-missing-target",
        context: {
          timezone: "Asia/Shanghai",
          current_focus: {
            type: "calendar_event",
            event_id: "calendar_1",
            title: "客户来访接待",
            start_time: "2026-06-06T09:30:00+08:00",
            end_time: "2026-06-06T10:30:00+08:00"
          }
        }
      })
    );

    expect(missingFocus.follow_up.reason).toBe("missing_meeting_focus");
    expect(missingTarget.follow_up.reason).toBe("missing_notification_target");
    expect(im.calls).toHaveLength(0);
  });

  it("reports failed late notification sends and keeps meeting focus for retry", async () => {
    const im = new FakeIMAdapter({
      ok: false,
      code: "SEND_FAILED",
      message: "permission denied"
    });
    const { handler } = createTestHandler({ im });
    const response = await handler.handleEvent(lateNotificationEvent({ event_id: "evt-late-fail" }));

    expect(response.actions[0]).toMatchObject({
      type: "lark.message.send",
      status: "failed",
      error: {
        code: "SEND_FAILED"
      }
    });
    expect(response.context_patch.current_focus).toMatchObject({
      event_id: "calendar_1"
    });
  });

  it("routes sedentary_detected events to wellbeing companion without calendar creation", async () => {
    const { handler, calendar } = createTestHandler();
    const response = await handler.handleEvent({
      ...sampleEvent,
      event_id: "evt-sedentary-route",
      type: "sedentary_detected",
      timestamp: "2026-06-06T13:40:00+08:00",
      payload: {
        duration_minutes: 35,
        confidence: 0.91
      }
    });

    expect(response.follow_up.expected).toBe(true);
    expect(response.actions.map((action) => action.type)).toEqual([
      "lark.calendar.list",
      "wellbeing.sedentary.evaluate"
    ]);
    expect(response.actions[1]).toMatchObject({
      status: "success",
      details: {
        decision: "allowed"
      }
    });
    expect(calendar.calls).toHaveLength(0);
    expect(calendar.listCalls).toHaveLength(1);
  });

  it("routes wellbeing_companion_requested events to wellbeing companion", async () => {
    const { handler, calendar } = createTestHandler();
    const response = await handler.handleEvent({
      ...sampleEvent,
      event_id: "evt-wellbeing-follow-up",
      type: "wellbeing_companion_requested",
      payload: {
        content_type: "relaxation"
      }
    });

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
    expect(calendar.calls).toHaveLength(0);
    expect(calendar.listCalls).toHaveLength(0);
  });

  it("still routes user_utterance calendar creation to CalendarAssistant", async () => {
    const { handler, calendar } = createTestHandler();
    const response = await handler.handleEvent({ ...sampleEvent, event_id: "evt-calendar-route" });
    expect(response.actions[0]?.type).toBe("lark.calendar.create");
    expect(calendar.calls).toHaveLength(1);
    expect(calendar.listCalls).toHaveLength(0);
  });

  it("returns unsupported-event responses without side effects", async () => {
    const { handler, calendar } = createTestHandler();
    const response = await handler.handleEvent({ ...sampleEvent, event_id: "evt-unsupported", type: "unknown_event" });
    expect(response.follow_up.expected).toBe(false);
    expect(response.actions).toEqual([]);
    expect(calendar.calls).toHaveLength(0);
    expect(calendar.listCalls).toHaveLength(0);
  });

  it("does not duplicate side effects for duplicate event ids", async () => {
    const { handler, calendar } = createTestHandler();
    const first = await handler.handleEvent(sampleEvent);
    const second = await handler.handleEvent(sampleEvent);
    expect(first).toEqual(second);
    expect(calendar.calls).toHaveLength(1);
  });

  it("reports adapter failures as failed actions", async () => {
    const calendar = new FakeCalendarAdapter({
      ok: false,
      code: "ADAPTER_FAILED",
      message: "boom"
    });
    const { handler } = createTestHandler({ calendar });
    const response = await handler.handleEvent({ ...sampleEvent, event_id: "evt-adapter-failure" });
    expect(response.actions[0]?.status).toBe("failed");
    expect(response.actions[0]?.error?.code).toBe("ADAPTER_FAILED");
  });

  it("bypasses parser requirements when a valid structured intent is present", async () => {
    const contact = new FakeContactAdapter({
      Gargantua: {
        status: "unique",
        person: { id: "ou_gargantua", name: "Gargantua" }
      }
    });
    const { handler, calendar } = createTestHandler({ contact });
    const response = await handler.handleEvent(structuredEvent());

    expect(response.follow_up.expected).toBe(false);
    expect(calendar.calls).toHaveLength(1);
    expect(calendar.calls[0]?.title).toBe("OpenClaw 测试");
    expect(calendar.calls[0]?.start).toBe("2026-06-06T10:00:00+08:00");
    expect(calendar.calls[0]?.end).toBe("2026-06-06T10:30:00+08:00");
    expect(calendar.calls[0]?.attendeeIds).toEqual(["ou_gargantua"]);
  });

  it("creates structured calendar events with direct attendee ids without contact lookup", async () => {
    const contact = new FakeContactAdapter({});
    const { handler, calendar } = createTestHandler({ contact });
    const response = await handler.handleEvent(
      structuredEvent({
        event_id: "evt-structured-direct-id",
        payload: {
          text: "随便说法也应该不影响结构化执行",
          structured_intent: {
            type: "calendar.create",
            version: "1",
            title: "直接 ID 测试",
            start: "2026-06-06T10:00:00+08:00",
            end: "2026-06-06T10:30:00+08:00",
            attendees: [{ id: "ou_direct" }, { id: "oc_chat" }, { id: "omm_meeting" }]
          }
        }
      })
    );

    expect(response.follow_up.expected).toBe(false);
    expect(contact.calls).toHaveLength(0);
    expect(calendar.calls[0]?.attendeeIds).toEqual(["ou_direct", "oc_chat", "omm_meeting"]);
  });

  it("returns follow-ups without side effects for invalid structured intents", async () => {
    const cases = [
      {
        event_id: "evt-structured-malformed",
        structured_intent: "calendar.create",
        reason: "malformed_structured_intent"
      },
      {
        event_id: "evt-structured-unsupported-type",
        structured_intent: {
          type: "task.create",
          version: "1",
          title: "Task",
          start: "2026-06-06T10:00:00+08:00",
          end: "2026-06-06T10:30:00+08:00",
          attendees: [{ name: "Gargantua" }]
        },
        reason: "unsupported_intent_type"
      },
      {
        event_id: "evt-structured-unsupported-version",
        structured_intent: {
          type: "calendar.create",
          version: "2",
          title: "OpenClaw 测试",
          start: "2026-06-06T10:00:00+08:00",
          end: "2026-06-06T10:30:00+08:00",
          attendees: [{ name: "Gargantua" }]
        },
        reason: "unsupported_intent_version"
      },
      {
        event_id: "evt-structured-missing-title",
        structured_intent: {
          type: "calendar.create",
          version: "1",
          start: "2026-06-06T10:00:00+08:00",
          end: "2026-06-06T10:30:00+08:00",
          attendees: [{ name: "Gargantua" }]
        },
        reason: "missing_required_structured_fields"
      },
      {
        event_id: "evt-structured-missing-attendees",
        structured_intent: {
          type: "calendar.create",
          version: "1",
          title: "OpenClaw 测试",
          start: "2026-06-06T10:00:00+08:00",
          end: "2026-06-06T10:30:00+08:00",
          attendees: []
        },
        reason: "missing_required_structured_fields"
      },
      {
        event_id: "evt-structured-invalid-time",
        structured_intent: {
          type: "calendar.create",
          version: "1",
          title: "OpenClaw 测试",
          start: "2026-06-06T10:30:00+08:00",
          end: "2026-06-06T10:00:00+08:00",
          attendees: [{ name: "Gargantua" }]
        },
        reason: "invalid_time_range"
      },
      {
        event_id: "evt-structured-invalid-attendee",
        structured_intent: {
          type: "calendar.create",
          version: "1",
          title: "OpenClaw 测试",
          start: "2026-06-06T10:00:00+08:00",
          end: "2026-06-06T10:30:00+08:00",
          attendees: [{ id: "user_bad" }]
        },
        reason: "invalid_attendee_reference"
      }
    ];

    for (const testCase of cases) {
      const contact = new FakeContactAdapter({});
      const calendar = new FakeCalendarAdapter();
      const { handler } = createTestHandler({ contact, calendar });
      const response = await handler.handleEvent(
        structuredEvent({
          event_id: testCase.event_id,
          payload: {
            text: "明天上午10点到10点半有个活动 OpenClaw 测试，帮我建一个飞书日程，邀请 Gargantua 参会",
            structured_intent: testCase.structured_intent
          }
        })
      );

      expect(response.follow_up.expected, testCase.event_id).toBe(true);
      expect(response.follow_up.reason, testCase.event_id).toBe(testCase.reason);
      expect(response.actions, testCase.event_id).toEqual([]);
      expect(contact.calls, testCase.event_id).toHaveLength(0);
      expect(calendar.calls, testCase.event_id).toHaveLength(0);
    }
  });

  it("returns structured follow-ups for ambiguous and missing structured attendees", async () => {
    const calendar = new FakeCalendarAdapter();
    const { handler } = createTestHandler({
      calendar,
      contact: new FakeContactAdapter({
        Gargantua: {
          status: "ambiguous",
          candidates: [
            { id: "ou_1", name: "Gargantua", department: "研发" },
            { id: "ou_2", name: "Gargantua", department: "销售" }
          ]
        }
      })
    });

    const ambiguous = await handler.handleEvent(structuredEvent({ event_id: "evt-structured-ambiguous" }));
    const missing = await handler.handleEvent(
      structuredEvent({
        event_id: "evt-structured-missing-person",
        payload: {
          text: "明天上午10点到10点半有个活动 OpenClaw 测试，帮我建一个飞书日程，邀请 Unknown 参会",
          structured_intent: {
            type: "calendar.create",
            version: "1",
            title: "OpenClaw 测试",
            start: "2026-06-06T10:00:00+08:00",
            end: "2026-06-06T10:30:00+08:00",
            attendees: [{ name: "Unknown" }]
          }
        }
      })
    );

    expect(ambiguous.follow_up.reason).toBe("ambiguous_attendee");
    expect(missing.follow_up.reason).toBe("missing_attendee");
    expect(calendar.calls).toHaveLength(0);
  });

  it("does not duplicate side effects for duplicate structured intent event ids", async () => {
    const contact = new FakeContactAdapter({
      Gargantua: {
        status: "unique",
        person: { id: "ou_gargantua", name: "Gargantua" }
      }
    });
    const { handler, calendar } = createTestHandler({ contact });
    const event = structuredEvent({ event_id: "evt-structured-duplicate" });

    const first = await handler.handleEvent(event);
    const second = await handler.handleEvent(event);

    expect(first).toEqual(second);
    expect(calendar.calls).toHaveLength(1);
  });

  it("does not duplicate Lark message side effects for duplicate notification event ids", async () => {
    const { handler, im } = createTestHandler();
    const event = lateNotificationEvent({ event_id: "evt-late-duplicate" });

    const first = await handler.handleEvent(event);
    const second = await handler.handleEvent(event);

    expect(first).toEqual(second);
    expect(im.calls).toHaveLength(1);
  });

  it("does not cache advisory travel responses as Lark write side effects", async () => {
    const { handler, route } = createTestHandler();
    const event = outdoorTravelEvent({ event_id: "evt-travel-duplicate" });

    const first = await handler.handleEvent(event);
    const second = await handler.handleEvent(event);

    expect(first.actions.at(-1)).toMatchObject({ type: "travel.plan.generate", status: "success" });
    expect(second.actions.at(-1)).toMatchObject({ type: "travel.plan.generate", status: "success" });
    expect(route.calls).toHaveLength(2);
  });
});

function structuredEvent(overrides: Record<string, unknown> = {}) {
  return {
    ...sampleEvent,
    event_id: "evt-structured",
    payload: {
      text: "明天上午10点到10点半有个活动 OpenClaw 测试，帮我建一个飞书日程，邀请 Gargantua 参会",
      structured_intent: {
        type: "calendar.create",
        version: "1",
        title: "OpenClaw 测试",
        start: "2026-06-06T10:00:00+08:00",
        end: "2026-06-06T10:30:00+08:00",
        attendees: [{ name: "Gargantua" }]
      }
    },
    ...overrides
  };
}

function meetingStartingSoonEvent(overrides: Record<string, unknown> = {}) {
  return {
    ...sampleEvent,
    event_id: "evt-meeting-starting-soon",
    type: "meeting_starting_soon",
    timestamp: "2026-06-06T09:20:00+08:00",
    payload: {
      trigger: {
        rule_id: "meeting_starting_soon",
        scheduled_for: "2026-06-06T01:20:00.000Z",
        fired_at: "2026-06-06T01:20:00.000Z",
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
        location: "上海办公室",
        notification_target: {
          chat_id: "oc_meeting_chat"
        }
      }
    },
    context: {
      timezone: "Asia/Shanghai"
    },
    ...overrides
  };
}

function lateNotificationEvent(overrides: Record<string, unknown> = {}) {
  return {
    ...sampleEvent,
    event_id: "evt-late-notify",
    payload: {
      text: "我会晚到五分钟，帮我通知参会人",
      structured_intent: {
        type: "meeting.notify_late",
        version: "1",
        delay_minutes: 5,
        message: "我会晚五分钟到，请大家稍等一下"
      }
    },
    context: {
      timezone: "Asia/Shanghai",
      current_focus: {
        type: "calendar_event",
        event_id: "calendar_1",
        calendar_id: "primary",
        title: "客户来访接待",
        start_time: "2026-06-06T09:30:00+08:00",
        end_time: "2026-06-06T10:30:00+08:00",
        location: "上海办公室",
        notification_target: {
          chat_id: "oc_meeting_chat"
        }
      }
    },
    ...overrides
  };
}

function outdoorTravelEvent(overrides: Record<string, unknown> = {}) {
  return {
    ...sampleEvent,
    event_id: "evt-outdoor-travel",
    type: "outdoor_event_detected",
    timestamp: "2026-06-06T17:00:00+08:00",
    payload: {
      trigger: {
        rule_id: "outdoor_event",
        scheduled_for: "2026-06-06T09:00:00.000Z",
        fired_at: "2026-06-06T09:00:00.000Z",
        source: "proactive_calendar_scheduler",
        trigger_key: "trigger_outdoor",
        calendar_id: "primary",
        source_event_id: "outdoor_1"
      },
      calendar_event: {
        id: "outdoor_1",
        title: "外出客户园区拜访",
        start: "2026-06-06T18:00:00+08:00",
        end: "2026-06-06T19:30:00+08:00",
        calendar_id: "primary",
        location: "客户园区",
        description: "带方案材料到现场沟通"
      }
    },
    ...overrides
  };
}

function businessTripEvent(overrides: Record<string, unknown> = {}) {
  return {
    ...sampleEvent,
    event_id: "evt-business-trip",
    type: "business_trip_tomorrow_detected",
    timestamp: "2026-06-06T18:00:00+08:00",
    payload: {
      trigger: {
        rule_id: "business_trip_tomorrow",
        scheduled_for: "2026-06-06T10:00:00.000Z",
        fired_at: "2026-06-06T10:00:00.000Z",
        source: "proactive_calendar_scheduler",
        trigger_key: "trigger_trip",
        calendar_id: "primary",
        source_event_id: "trip_1"
      },
      calendar_event: {
        id: "trip_1",
        title: "北京出差",
        start: "2026-06-07T09:00:00+08:00",
        end: "2026-06-07T20:00:00+08:00",
        calendar_id: "primary",
        location: "北京",
        description: "航班和客户会议"
      }
    },
    ...overrides
  };
}
