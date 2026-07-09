import { mkdtemp, rm } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { describe, expect, it } from "vitest";
import type { InputEvent, StructuredResponse } from "../src/contracts.js";
import {
  JsonFileTriggerPlanStore,
  MemoryTriggerPlanStore,
  ProactiveCalendarTriggerScheduler,
  calculateScanWindow,
  createBusinessTripTomorrowPlans,
  createDailyBriefingPlans,
  createMeetingStartingSoonPlans,
  createOutdoorEventPlans,
  buildDynamicXiaopaiSessionKey,
  buildOpenClawCronAddArgs,
  buildSchedulerAgentTurnMessage,
  dispatchSchedulerResponseToAgent,
  selectOnlineXiaopaiDeviceId,
  deriveInputEventId,
  deriveTriggerKey,
  readSchedulerConfig
} from "../src/scheduler/index.js";
import type { ProactiveCalendarSchedulerConfig, SchedulerAgentTurnScheduleParams, TriggerPlan } from "../src/scheduler/index.js";
import { DryRunCalendarAdapter } from "../src/lark/dry-run.js";
import { FakeCalendarAdapter, sampleTodayEvents } from "./helpers.js";
import { createDefaultWorkAssistantRuntime } from "../src/index.js";

const schedulerConfig: ProactiveCalendarSchedulerConfig = {
  enabled: true,
  startIntervalLoop: false,
  scanIntervalMs: 600000,
  lookaheadHours: 48,
  timezone: "Asia/Shanghai",
  userId: "ou_requester",
  calendarId: "primary",
  maxDispatchAttempts: 3,
  agentDispatch: {
    enabled: false,
    deliveryMode: "none",
    interrupt: true
  },
  rules: {
    dailyBriefing: { enabled: true, localTime: "08:00" },
    meetingStartingSoon: {
      enabled: true,
      offsetMinutes: 10,
      keywords: ["会议", "同步", "meeting"]
    },
    outdoorEvent: {
      enabled: true,
      offsetMinutes: 60,
      keywords: ["外出", "客户", "园区", "site"]
    },
    businessTripTomorrow: {
      enabled: true,
      localTime: "18:00",
      keywords: ["出差", "航班", "travel"]
    }
  }
};

const successResponse: StructuredResponse = {
  speech: "ok",
  presentation: {},
  actions: [],
  follow_up: { expected: false },
  context_patch: {}
};

describe("scheduler config", () => {
  it("defaults to disabled scheduler with safe rule enablement", () => {
    const config = readSchedulerConfig(undefined);
    expect(config.enabled).toBe(false);
    expect(config.scanIntervalMs).toBe(600000);
    expect(config.lookaheadHours).toBe(48);
    expect(config.timezone).toBe("Asia/Shanghai");
    expect(config.rules.dailyBriefing.enabled).toBe(true);
    expect(config.rules.meetingStartingSoon.enabled).toBe(false);
    expect(config.rules.outdoorEvent.enabled).toBe(false);
    expect(config.rules.businessTripTomorrow.enabled).toBe(false);
    expect(config.agentDispatch).toEqual({
      enabled: false,
      deliveryMode: "none",
      interrupt: true
    });
  });

  it("parses scheduler config and falls back from invalid optional values", () => {
    const config = readSchedulerConfig({
      enabled: true,
      scanIntervalMs: -1,
      lookaheadHours: 24,
      timezone: "UTC",
      userId: "ou_user",
      calendarId: "cal_1",
      statePath: "/tmp/state.json",
      agentDispatch: {
        enabled: true,
        sessionKey: "agent:main:xiaopai-{device_id}",
        sessionKeyMode: "online_xiaopai",
        agentId: "work-agent",
        deliveryMode: "announce",
        deviceId: "device-1",
        xiaopaiBaseUrl: "http://127.0.0.1:8091",
        xiaopaiDeviceLookupTimeoutMs: 1500,
        interrupt: false
      },
      rules: {
        dailyBriefing: { enabled: false, localTime: "bad" },
        meetingStartingSoon: { enabled: true, offsetMinutes: 5, keywords: ["demo", "demo"] }
      }
    });
    expect(config).toMatchObject({
      enabled: true,
      scanIntervalMs: 600000,
      lookaheadHours: 24,
      timezone: "UTC",
      userId: "ou_user",
      calendarId: "cal_1",
      statePath: "/tmp/state.json",
      agentDispatch: {
        enabled: true,
        sessionKey: "agent:main:xiaopai-{device_id}",
        sessionKeyMode: "online_xiaopai",
        agentId: "work-agent",
        deliveryMode: "announce",
        deviceId: "device-1",
        xiaopaiBaseUrl: "http://127.0.0.1:8091",
        xiaopaiDeviceLookupTimeoutMs: 1500,
        interrupt: false
      }
    });
    expect(config.rules.dailyBriefing).toEqual({ enabled: false, localTime: "08:00" });
    expect(config.rules.meetingStartingSoon).toMatchObject({
      enabled: true,
      offsetMinutes: 5,
      keywords: ["demo"]
    });
  });
});

describe("scheduler agent dispatch prompt", () => {
  it("selects a realtime online Xiaopai device and builds a matching OpenClaw session key", () => {
    const deviceId = selectOnlineXiaopaiDeviceId({
      default_device_id: "default",
      devices: [{ device_id: "http-stale", online: true }],
      realtime_devices: [
        { device_id: "44:1b:f6:df:5d:b8", online: true },
        { device_id: "44:1b:f6:e4:83:8c", online: false }
      ]
    });
    expect(deviceId).toBe("44:1b:f6:df:5d:b8");
    expect(
      buildDynamicXiaopaiSessionKey(
        {
          sessionKey: "agent:main:xiaopai-{device_id}",
          agentId: "main"
        },
        deviceId ?? ""
      )
    ).toBe("agent:main:xiaopai-44:1b:f6:df:5d:b8");
  });

  it("builds CLI fallback cron args with the current duration syntax", () => {
    const args = buildOpenClawCronAddArgs({
      sessionKey: "agent:main:xiaopai-device-1",
      message: "hello",
      delayMs: 0,
      deleteAfterRun: true,
      deliveryMode: "none",
      tag: "work-assistant-scheduler",
      name: "scheduler_test",
      agentId: "main"
    });
    expect(args[args.indexOf("--at") + 1]).toBe("1s");
    expect(args).not.toContain("+1s");
    expect(args).toEqual(
      expect.arrayContaining([
        "cron",
        "add",
        "--delete-after-run",
        "--session-key",
        "agent:main:xiaopai-device-1",
        "--light-context",
        "--thinking",
        "low",
        "--no-deliver",
        "--agent",
        "main"
      ])
    );
  });

  it("builds a direct Xiaopai control prompt around the StructuredResponse", () => {
    const event: InputEvent = {
      event_id: "scheduler-event-1",
      type: "meeting_starting_soon",
      timestamp: "2026-06-06T09:20:00+08:00",
      user_id: "ou_requester",
      payload: {
        trigger: {
          rule_id: "meeting_starting_soon",
          scheduled_for: "2026-06-06T01:20:00.000Z",
          fired_at: "2026-06-06T01:20:00.000Z",
          source: "proactive_calendar_scheduler",
          trigger_key: "trigger_1"
        },
        calendar_event: {
          id: "calendar-event-1",
          title: "客户同步会议",
          start: "2026-06-06T09:30:00+08:00",
          end: "2026-06-06T10:00:00+08:00",
          location: "3A 会议室"
        }
      },
      context: { timezone: "Asia/Shanghai" }
    };

    const message = buildSchedulerAgentTurnMessage({
      event,
      response: successResponse,
      config: {
        enabled: true,
        sessionKey: "xiaopai-device-1",
        deliveryMode: "none",
        deviceId: "device-1",
        interrupt: true
      }
    });

    expect(() => JSON.parse(message)).toThrow();
    expect(message).not.toContain("openclaw.stackchan.event.v1");
    expect(message).toContain("必须调用 xiaopaiControl.execute");
    expect(message).toContain("不要调用 workAssistant.handleEvent");
    expect(message).toContain("任务类型: 会议会前提醒 (meeting_starting_soon)");
    expect(message).toContain("目标小派设备 device_id: device-1");
    expect(message).toContain("打断当前播报 interrupt: true");
    expect(message).toContain("日程标题: 客户同步会议");
    expect(message).toContain("日程地点: 3A 会议室");
    expect(message).toContain("距离日程开始: 10 分钟");
    expect(message).toContain("打扰一下，xxx点 xx 会议还有 x 分钟");
    expect(message).toContain("face expression: smile_blink");
    expect(message).toContain("action: blink");
  });

  it("adds task-specific speech styles and Xiaopai presentation instructions", () => {
    const baseEvent: InputEvent = {
      event_id: "scheduler-profile-event",
      type: "daily_briefing_triggered",
      timestamp: "2026-06-06T08:00:00+08:00",
      user_id: "ou_requester",
      payload: {
        trigger: {
          rule_id: "daily_briefing",
          scheduled_for: "2026-06-06T00:00:00.000Z",
          fired_at: "2026-06-06T00:00:00.000Z",
          source: "proactive_calendar_scheduler",
          trigger_key: "trigger_profile"
        }
      },
      context: { timezone: "Asia/Shanghai" }
    };
    const build = (event: InputEvent, response = successResponse) =>
      buildSchedulerAgentTurnMessage({
        event,
        response,
        config: {
          enabled: true,
          sessionKey: "xiaopai-device-1",
          deliveryMode: "none",
          deviceId: "device-1",
          interrupt: true
        }
      });

    expect(build(baseEvent)).toEqual(expect.stringContaining("早上好呀！今天是x月x日（周x）"));
    expect(build(baseEvent)).toEqual(expect.stringContaining("face expression: happy_squint"));

    expect(
      build({
        ...baseEvent,
        type: "meeting_starting_soon",
        payload: {
          ...baseEvent.payload,
          calendar_event: {
            id: "meeting-1",
            title: "项目同步会议",
            start: "2026-06-06T08:05:00+08:00",
            end: "2026-06-06T09:00:00+08:00",
            location: "B2"
          }
        }
      })
    ).toEqual(expect.stringContaining("face expression: smile_blink 和 action: blink"));

    expect(
      build({
        ...baseEvent,
        type: "outdoor_event_detected",
        payload: {
          ...baseEvent.payload,
          calendar_event: {
            id: "outdoor-1",
            title: "外出拜访",
            start: "2026-06-06T14:00:00+08:00",
            end: "2026-06-06T16:00:00+08:00",
            location: "深圳湾客户园区"
          }
        }
      })
    ).toEqual(expect.stringContaining("哈喽，您下午 xxx 点有外出行程"));

    expect(
      build(
        {
          ...baseEvent,
          type: "business_trip_tomorrow_detected",
          payload: {
            ...baseEvent.payload,
            calendar_event: {
              id: "trip-1",
              title: "出差 - 上海",
              start: "2026-06-07T09:00:00+08:00",
              end: "2026-06-07T11:00:00+08:00",
              description: "前往上海出差"
            }
          }
        },
        {
          ...successResponse,
          context_patch: {
            travel_summary: {
              destination: "上海",
              trip_date: "2026-06-07",
              weather_status: "上海明日小雨，23到31度。"
            }
          }
        }
      )
    ).toEqual(expect.stringContaining("哈喽，准备下班咯~提醒您明早 x 点将前往 xx 出差"));
  });

  it("resolves online Xiaopai device into both scheduler session key and prompt target", async () => {
    const scheduledTurns: SchedulerAgentTurnScheduleParams[] = [];
    const event: InputEvent = {
      event_id: "scheduler-event-online-device",
      type: "meeting_starting_soon",
      timestamp: "2026-06-06T09:20:00+08:00",
      user_id: "ou_requester",
      payload: {
        trigger: {
          rule_id: "meeting_starting_soon",
          scheduled_for: "2026-06-06T01:20:00.000Z",
          fired_at: "2026-06-06T01:20:00.000Z",
          source: "proactive_calendar_scheduler",
          trigger_key: "trigger_online_device"
        }
      },
      context: { timezone: "Asia/Shanghai" }
    };

    const result = await dispatchSchedulerResponseToAgent({
      event,
      response: successResponse,
      config: {
        enabled: true,
        sessionKey: "agent:main:xiaopai-{device_id}",
        sessionKeyMode: "online_xiaopai",
        agentId: "main",
        deliveryMode: "none",
        interrupt: true
      },
      api: {
        async resolveOnlineXiaopaiDeviceId() {
          return "44:1b:f6:df:5d:b8";
        },
        session: {
          workflow: {
            async scheduleSessionTurn(params: SchedulerAgentTurnScheduleParams) {
              scheduledTurns.push(params);
              return {
                id: "job_online_device",
                pluginId: "work-assistant",
                sessionKey: params.sessionKey,
                kind: "session-turn"
              };
            }
          }
        }
      }
    });

    expect(result).toMatchObject({
      status: "success",
      sessionKey: "agent:main:xiaopai-44:1b:f6:df:5d:b8"
    });
    expect(scheduledTurns).toHaveLength(1);
    expect(scheduledTurns[0]?.sessionKey).toBe("agent:main:xiaopai-44:1b:f6:df:5d:b8");
    expect(scheduledTurns[0]?.message).toContain("目标小派设备 device_id: 44:1b:f6:df:5d:b8");
    expect(scheduledTurns[0]?.message).toContain('"device_id": "44:1b:f6:df:5d:b8"');
    expect(scheduledTurns[0]?.message).toContain("必须调用 xiaopaiControl.execute");
  });
});

describe("scheduler scanning and rules", () => {
  it("calculates bounded scan windows from the scheduler timestamp", () => {
    expect(calculateScanWindow(new Date("2026-06-06T00:00:00.000Z"), 2)).toEqual({
      start: "2026-06-06T00:00:00.000Z",
      end: "2026-06-06T02:00:00.000Z"
    });
  });

  it("records successful calendar scans and generated plan counts", async () => {
    const calendar = new FakeCalendarAdapter(undefined, {
      ok: true,
      calendarId: "primary",
      events: sampleTodayEvents
    });
    const scheduler = new ProactiveCalendarTriggerScheduler({
      config: schedulerConfig,
      calendarAdapter: calendar,
      store: new MemoryTriggerPlanStore(),
      dispatch: async () => successResponse
    });

    const result = await scheduler.refresh(new Date("2026-06-06T00:00:00.000Z"));
    expect(result).toMatchObject({
      ok: true,
      type: "proactive.calendar.scan",
      calendarId: "primary",
      eventCount: 3,
      planCount: 4,
      upserted: 4
    });
    expect(calendar.listCalls[0]).toEqual({
      start: "2026-06-06T00:00:00.000Z",
      end: "2026-06-08T00:00:00.000Z",
      calendarId: "primary"
    });
  });

  it("records failed scans without deleting existing pending plans", async () => {
    const store = new MemoryTriggerPlanStore();
    const seed = createDailyBriefingPlans({
      now: new Date("2026-06-06T00:00:00.000Z"),
      timezone: "Asia/Shanghai",
      userId: "ou_requester",
      calendarId: "primary",
      maxDispatchAttempts: 3,
      rules: schedulerConfig.rules
    });
    await store.upsertPlans(seed, "2026-06-06T00:00:00.000Z");
    const scheduler = new ProactiveCalendarTriggerScheduler({
      config: schedulerConfig,
      calendarAdapter: new FakeCalendarAdapter(undefined, {
        ok: false,
        code: "LIST_FAILED",
        message: "boom"
      }),
      store,
      dispatch: async () => successResponse
    });

    const result = await scheduler.refresh(new Date("2026-06-06T00:00:00.000Z"));
    expect(result).toMatchObject({
      ok: false,
      type: "proactive.calendar.scan",
      code: "LIST_FAILED",
      message: "boom"
    });
    expect(await store.listRecords()).toHaveLength(1);
  });

  it("generates each enabled trigger rule with scheduler-produced payload data", () => {
    const context = {
      now: new Date("2026-06-06T00:00:00.000Z"),
      timezone: "Asia/Shanghai",
      userId: "ou_requester",
      calendarId: "primary",
      maxDispatchAttempts: 3,
      rules: schedulerConfig.rules
    };
    expect(createDailyBriefingPlans(context)[0]).toMatchObject({
      type: "daily_briefing_triggered",
      scheduledFor: "2026-06-06T00:00:00.000Z"
    });
    expect(createMeetingStartingSoonPlans({ ...context, events: sampleTodayEvents })[0]).toMatchObject({
      type: "meeting_starting_soon",
      scheduledFor: "2026-06-06T01:20:00.000Z",
      calendarEvent: {
        title: "客户来访接待",
        notificationTarget: {
          chatId: "oc_customer_visit"
        }
      }
    });
    expect(createOutdoorEventPlans({ ...context, events: sampleTodayEvents })[0]).toMatchObject({
      type: "outdoor_event_detected",
      calendarEvent: { title: "客户来访接待" }
    });
    expect(
      createBusinessTripTomorrowPlans({
        ...context,
        events: [
          {
            id: "trip",
            title: "北京出差",
            start: "2026-06-07T09:00:00+08:00",
            end: "2026-06-07T20:00:00+08:00"
          }
        ]
      })[0]
    ).toMatchObject({
      type: "business_trip_tomorrow_detected",
      scheduledFor: "2026-06-06T10:00:00.000Z",
      calendarEvent: { title: "北京出差" }
    });
  });

  it("keeps unsupported future rules disabled unless explicitly configured", () => {
    const config = readSchedulerConfig({ enabled: true });
    expect(config.rules.meetingStartingSoon.enabled).toBe(false);
    const plans = createMeetingStartingSoonPlans({
      now: new Date("2026-06-06T00:00:00.000Z"),
      timezone: config.timezone,
      userId: config.userId,
      calendarId: config.calendarId,
      maxDispatchAttempts: config.maxDispatchAttempts,
      rules: config.rules,
      events: sampleTodayEvents
    });
    expect(plans).toHaveLength(0);
  });
});

describe("scheduler store and dispatch", () => {
  it("claims due records until they are dispatched or fail", async () => {
    const store = new MemoryTriggerPlanStore();
    await store.upsertPlans([planFixture({ key: "claim", scheduledFor: "2026-06-06T00:00:00.000Z" })], "2026-06-05T23:00:00.000Z");

    expect(await store.getDue("2026-06-06T00:00:00.000Z")).toMatchObject([{ key: "claim" }]);
    expect(await store.getDue("2026-06-06T00:00:01.000Z")).toHaveLength(0);

    await store.recordDispatchFailure("claim", "transient", "2026-06-06T00:00:02.000Z");
    expect(await store.getDue("2026-06-06T00:00:03.000Z")).toMatchObject([
      {
        key: "claim",
        attempts: 1,
        lastDispatchError: "transient"
      }
    ]);
  });

  it("shares JSON state claims between store instances for one gateway process", async () => {
    const dir = await mkdtemp(join(tmpdir(), "work-assistant-scheduler-claims-"));
    try {
      const statePath = join(dir, "state.json");
      const firstStore = new JsonFileTriggerPlanStore(statePath);
      const secondStore = new JsonFileTriggerPlanStore(statePath);
      await firstStore.upsertPlans(
        [planFixture({ key: "shared-claim", scheduledFor: "2026-06-06T00:00:00.000Z" })],
        "2026-06-05T23:00:00.000Z"
      );

      expect(await firstStore.getDue("2026-06-06T00:00:00.000Z")).toMatchObject([{ key: "shared-claim" }]);
      expect(await secondStore.getDue("2026-06-06T00:00:01.000Z")).toHaveLength(0);

      await firstStore.markDispatched("shared-claim", "event_shared_claim", "2026-06-06T00:00:02.000Z", successResponse);
      expect(await secondStore.listRecords()).toMatchObject([
        {
          key: "shared-claim",
          status: "dispatched",
          lastEventId: "event_shared_claim"
        }
      ]);
    } finally {
      await rm(dir, { recursive: true, force: true });
    }
  });

  it("upserts repeated scans and replaces stale pending times for moved events", async () => {
    const store = new MemoryTriggerPlanStore();
    const first = planFixture({
      key: "k1",
      updateGroupKey: "group",
      scheduledFor: "2026-06-06T01:20:00.000Z",
      eventHash: "hash1"
    });
    const second = planFixture({
      key: "k2",
      updateGroupKey: "group",
      scheduledFor: "2026-06-06T01:50:00.000Z",
      eventHash: "hash2"
    });
    expect(await store.upsertPlans([first], "2026-06-06T00:00:00.000Z")).toEqual({
      upserted: 1,
      replacedPending: 0
    });
    expect(await store.upsertPlans([first], "2026-06-06T00:01:00.000Z")).toEqual({
      upserted: 1,
      replacedPending: 0
    });
    expect(await store.upsertPlans([second], "2026-06-06T00:02:00.000Z")).toEqual({
      upserted: 1,
      replacedPending: 1
    });
    expect((await store.listRecords()).map((record) => record.key)).toEqual(["k2"]);
  });

  it("reloads dispatched records from a JSON state file", async () => {
    const dir = await mkdtemp(join(tmpdir(), "work-assistant-scheduler-"));
    try {
      const statePath = join(dir, "state.json");
      const store = new JsonFileTriggerPlanStore(statePath);
      const plan = planFixture({ key: "persisted" });
      await store.upsertPlans([plan], "2026-06-06T00:00:00.000Z");
      await store.markDispatched("persisted", "event_persisted", "2026-06-06T01:00:00.000Z", successResponse);

      const reloaded = new JsonFileTriggerPlanStore(statePath);
      expect(await reloaded.getDue("2026-06-06T02:00:00.000Z")).toHaveLength(0);
      expect(await reloaded.listRecords()).toMatchObject([
        {
          key: "persisted",
          status: "dispatched",
          lastEventId: "event_persisted",
          dispatchedAt: "2026-06-06T01:00:00.000Z"
        }
      ]);
    } finally {
      await rm(dir, { recursive: true, force: true });
    }
  });

  it("dispatches due events through the handler boundary and marks success", async () => {
    const store = new MemoryTriggerPlanStore();
    await store.upsertPlans([planFixture({ scheduledFor: "2026-06-06T00:00:00.000Z" })], "2026-06-05T23:00:00.000Z");
    const dispatched: InputEvent[] = [];
    const scheduler = new ProactiveCalendarTriggerScheduler({
      config: schedulerConfig,
      calendarAdapter: new FakeCalendarAdapter(),
      store,
      dispatch: async (event) => {
        dispatched.push(event);
        return successResponse;
      }
    });

    const result = await scheduler.dispatchDue(new Date("2026-06-06T00:00:00.000Z"));
    expect(result).toMatchObject([{ status: "success", type: "daily_briefing_triggered" }]);
    expect(dispatched[0]).toMatchObject({
      event_id: "event_1",
      type: "daily_briefing_triggered",
      payload: {
        trigger: {
          rule_id: "daily_briefing",
          source: "proactive_calendar_scheduler",
          scheduled_for: "2026-06-06T00:00:00.000Z",
          fired_at: "2026-06-06T00:00:00.000Z"
        }
      },
      context: { timezone: "Asia/Shanghai" }
    });
    expect(await scheduler.dispatchDue(new Date("2026-06-06T00:01:00.000Z"))).toHaveLength(0);
  });

  it("records dispatch failures and leaves plans retryable", async () => {
    const store = new MemoryTriggerPlanStore();
    await store.upsertPlans([planFixture({ key: "retry", scheduledFor: "2026-06-06T00:00:00.000Z" })], "2026-06-05T23:00:00.000Z");
    const scheduler = new ProactiveCalendarTriggerScheduler({
      config: schedulerConfig,
      calendarAdapter: new FakeCalendarAdapter(),
      store,
      dispatch: async () => ({ ok: false, message: "not yet" })
    });

    const result = await scheduler.dispatchDue(new Date("2026-06-06T00:00:00.000Z"));
    expect(result).toMatchObject([{ status: "failed", error: "not yet" }]);
    expect(await store.getDue("2026-06-06T00:01:00.000Z")).toMatchObject([
      {
        key: "retry",
        attempts: 1,
        lastDispatchError: "not yet"
      }
    ]);
  });

  it("derives deterministic trigger keys and proactive event ids", () => {
    const key = deriveTriggerKey({
      userId: "ou_requester",
      calendarId: "primary",
      sourceEventId: "event_1",
      ruleId: "meeting_starting_soon",
      scheduledFor: "2026-06-06T01:20:00.000Z"
    });
    expect(key).toEqual(
      deriveTriggerKey({
        userId: "ou_requester",
        calendarId: "primary",
        sourceEventId: "event_1",
        ruleId: "meeting_starting_soon",
        scheduledFor: "2026-06-06T01:20:00.000Z"
      })
    );
    expect(deriveInputEventId(key)).toEqual(deriveInputEventId(key));
    expect(
      deriveTriggerKey({
        userId: "ou_requester",
        calendarId: "primary",
        sourceEventId: "event_1",
        ruleId: "meeting_starting_soon",
        scheduledFor: "2026-06-06T01:50:00.000Z"
      })
    ).not.toEqual(key);
  });
});

describe("dry-run scheduler smoke", () => {
  it("dispatches a scheduler-enabled dry-run daily briefing through the agenda handler", async () => {
    const dir = await mkdtemp(join(tmpdir(), "work-assistant-scheduler-smoke-"));
    try {
      const runtime = createDefaultWorkAssistantRuntime({
        pluginConfig: {
          dryRun: true,
          scheduler: {
            enabled: true,
            userId: "ou_requester",
            calendarId: "primary",
            statePath: join(dir, "scheduler.json"),
            rules: {
              dailyBriefing: { enabled: true, localTime: "08:00" }
            }
          }
        }
      });

      const result = await runtime.scheduler?.tick(new Date("2026-06-06T00:00:00.000Z"));
      expect(result?.scan).toMatchObject({ ok: true, planCount: 1 });
      expect(result?.dispatches).toMatchObject([{ status: "success", type: "daily_briefing_triggered" }]);
      const records = await runtime.scheduler?.listRecords();
      expect(records).toMatchObject([
        {
          status: "dispatched",
          responseSummary: {
            actionCount: 3,
            followUpExpected: false
          }
        }
      ]);
    } finally {
      await rm(dir, { recursive: true, force: true });
    }
  });

  it("queues a scheduler-produced StructuredResponse to the OpenClaw agent layer when configured", async () => {
    const dir = await mkdtemp(join(tmpdir(), "work-assistant-scheduler-agent-dispatch-"));
    const scheduledTurns: SchedulerAgentTurnScheduleParams[] = [];
    try {
      const runtime = createDefaultWorkAssistantRuntime({
        pluginConfig: {
          dryRun: true,
          scheduler: {
            enabled: true,
            userId: "ou_requester",
            calendarId: "primary",
            statePath: join(dir, "scheduler.json"),
            agentDispatch: {
              enabled: true,
              sessionKey: "xiaopai-device-1",
              deviceId: "device-1",
              interrupt: true
            },
            rules: {
              dailyBriefing: { enabled: true, localTime: "08:00" }
            }
          }
        },
        session: {
          workflow: {
            async scheduleSessionTurn(params: SchedulerAgentTurnScheduleParams) {
              scheduledTurns.push(params);
              return {
                id: "job_1",
                pluginId: "work-assistant",
                sessionKey: params.sessionKey,
                kind: "session-turn"
              };
            }
          }
        }
      });

      const result = await runtime.scheduler?.tick(new Date("2026-06-06T00:00:00.000Z"));
      expect(result?.dispatches).toMatchObject([{ status: "success", type: "daily_briefing_triggered" }]);
      expect(scheduledTurns).toHaveLength(1);
      const scheduledTurn = scheduledTurns[0];
      if (!scheduledTurn) throw new Error("Expected scheduler to queue an agent turn.");
      expect(scheduledTurn).toMatchObject({
        sessionKey: "xiaopai-device-1",
        delayMs: 0,
        deleteAfterRun: true,
        deliveryMode: "none",
        tag: "work-assistant-scheduler"
      });

      expect(() => JSON.parse(scheduledTurn.message)).toThrow();
      expect(scheduledTurn.message).not.toContain("openclaw.stackchan.event.v1");
      expect(scheduledTurn.message).toContain("任务类型: 定时日程管理及播报 (daily_briefing_triggered)");
      expect(scheduledTurn.message).toContain("目标小派设备 device_id: device-1");
      expect(scheduledTurn.message).toContain("早上好呀！今天是x月x日（周x）");
      expect(scheduledTurn.message).toContain("face expression: happy_squint");
      expect(scheduledTurn.message).toContain("原始播报:");
      expect(scheduledTurn.message).toContain("今天");
    } finally {
      await rm(dir, { recursive: true, force: true });
    }
  });

  it("dispatches a scheduler-enabled dry-run meeting reminder through the meeting handler", async () => {
    const dir = await mkdtemp(join(tmpdir(), "work-assistant-scheduler-meeting-smoke-"));
    try {
      const runtime = createDefaultWorkAssistantRuntime({
        pluginConfig: {
          dryRun: true,
          scheduler: {
            enabled: true,
            userId: "ou_requester",
            calendarId: "primary",
            statePath: join(dir, "scheduler.json"),
            rules: {
              dailyBriefing: { enabled: false },
              meetingStartingSoon: {
                enabled: true,
                offsetMinutes: 10,
                keywords: ["客户", "同步"]
              }
            }
          }
        }
      });

      const result = await runtime.scheduler?.tick(new Date("2026-06-06T01:20:00.000Z"));
      expect(result?.scan).toMatchObject({ ok: true, planCount: 4 });
      expect(result?.dispatches).toEqual([
        expect.objectContaining({
          status: "success",
          type: "meeting_starting_soon"
        })
      ]);
      const records = await runtime.scheduler?.listRecords();
      expect(records?.find((record) => record.type === "meeting_starting_soon" && record.status === "dispatched")).toMatchObject({
        status: "dispatched",
        responseSummary: {
          actionCount: 1,
          followUpExpected: false
        }
      });
    } finally {
      await rm(dir, { recursive: true, force: true });
    }
  });

  it("dispatches a scheduler-enabled dry-run outdoor event through the travel planner handler", async () => {
    const dir = await mkdtemp(join(tmpdir(), "work-assistant-scheduler-outdoor-smoke-"));
    try {
      const runtime = createDefaultWorkAssistantRuntime({
        pluginConfig: {
          dryRun: true,
          scheduler: {
            enabled: true,
            userId: "ou_requester",
            calendarId: "primary",
            statePath: join(dir, "scheduler.json"),
            rules: {
              dailyBriefing: { enabled: false },
              outdoorEvent: {
                enabled: true,
                offsetMinutes: 60,
                keywords: ["外出", "园区"]
              }
            }
          }
        }
      });

      const result = await runtime.scheduler?.tick(new Date("2026-06-06T09:00:00.000Z"));
      expect(result?.scan).toMatchObject({ ok: true, planCount: 1 });
      expect(result?.dispatches).toEqual([
        expect.objectContaining({
          status: "success",
          type: "outdoor_event_detected"
        })
      ]);
      const records = await runtime.scheduler?.listRecords();
      expect(records?.find((record) => record.type === "outdoor_event_detected" && record.status === "dispatched")).toMatchObject({
        status: "dispatched",
        responseSummary: {
          actionCount: 3,
          followUpExpected: false
        }
      });
    } finally {
      await rm(dir, { recursive: true, force: true });
    }
  });

  it("dispatches a scheduler-enabled dry-run business trip through the travel planner handler", async () => {
    const dir = await mkdtemp(join(tmpdir(), "work-assistant-scheduler-trip-smoke-"));
    try {
      const runtime = createDefaultWorkAssistantRuntime({
        pluginConfig: {
          dryRun: true,
          scheduler: {
            enabled: true,
            userId: "ou_requester",
            calendarId: "primary",
            statePath: join(dir, "scheduler.json"),
            rules: {
              dailyBriefing: { enabled: false },
              businessTripTomorrow: {
                enabled: true,
                localTime: "18:00",
                keywords: ["出差", "航班"]
              }
            }
          }
        }
      });

      const result = await runtime.scheduler?.tick(new Date("2026-06-06T10:00:00.000Z"));
      expect(result?.scan).toMatchObject({ ok: true, planCount: 1 });
      expect(result?.dispatches).toEqual([
        expect.objectContaining({
          status: "success",
          type: "business_trip_tomorrow_detected"
        })
      ]);
      const records = await runtime.scheduler?.listRecords();
      expect(records?.find((record) => record.type === "business_trip_tomorrow_detected" && record.status === "dispatched")).toMatchObject({
        status: "dispatched",
        responseSummary: {
          actionCount: 2,
          followUpExpected: false
        }
      });
    } finally {
      await rm(dir, { recursive: true, force: true });
    }
  });
});

function planFixture(overrides: Partial<TriggerPlan> = {}): TriggerPlan {
  return {
    key: "plan_1",
    updateGroupKey: "plan_group_1",
    eventId: "event_1",
    ruleId: "daily_briefing",
    type: "daily_briefing_triggered",
    userId: "ou_requester",
    calendarId: "primary",
    scheduledFor: "2026-06-06T00:00:00.000Z",
    eventHash: "hash",
    maxAttempts: 3,
    ...overrides
  };
}
