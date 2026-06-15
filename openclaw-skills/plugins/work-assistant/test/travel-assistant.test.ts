import { describe, expect, it } from "vitest";
import type { InputEvent } from "../src/contracts.js";
import { TravelPlannerAssistant } from "../src/travel/assistant.js";
import { FakeRouteAdapter, FakeUserProfileAdapter, FakeWeatherAdapter, sampleEvent } from "./helpers.js";

describe("travel planner assistant", () => {
  it("generates a route-aware outdoor reminder with a recommended departure time", async () => {
    const route = new FakeRouteAdapter();
    const assistant = createTravelAssistant({ route });

    const response = await assistant.handleOutdoorEvent(outdoorTravelEvent());

    expect(response.follow_up.expected).toBe(false);
    expect(response.speech).toContain("客户园区");
    expect(response.speech).toContain("42 分钟");
    expect(response.speech).toContain("17:03");
    expect(response.context_patch.current_focus).toMatchObject({
      type: "travel_event",
      event_id: "outdoor_1",
      destination: "客户园区",
      recommended_departure_time: "2026-06-06T09:03:00.000Z"
    });
    expect(response.actions).toEqual([
      expect.objectContaining({ type: "user.profile.read", status: "success" }),
      expect.objectContaining({
        type: "route.estimate",
        status: "success",
        details: expect.objectContaining({
          duration_minutes: 42,
          arrival_buffer_minutes: 15,
          trigger_key: "trigger_outdoor"
        })
      }),
      expect.objectContaining({
        type: "travel.plan.generate",
        status: "success",
        details: expect.objectContaining({
          recommended_departure_time: "2026-06-06T09:03:00.000Z"
        })
      })
    ]);
    expect(route.calls).toHaveLength(1);
  });

  it("degrades outdoor reminders without a destination and skips route lookup", async () => {
    const route = new FakeRouteAdapter();
    const assistant = createTravelAssistant({ route });
    const response = await assistant.handleOutdoorEvent(
      outdoorTravelEvent({
        payload: {
          trigger: {
            rule_id: "outdoor_event",
            scheduled_for: "2026-06-06T09:00:00.000Z",
            fired_at: "2026-06-06T09:00:00.000Z",
            source: "proactive_calendar_scheduler",
            trigger_key: "trigger_missing_destination"
          },
          calendar_event: {
            id: "outdoor_missing_destination",
            title: "外出安排",
            start: "2026-06-06T18:00:00+08:00",
            end: "2026-06-06T19:30:00+08:00"
          }
        }
      })
    );

    expect(response.speech).toContain("没有明确地点");
    expect(response.actions).toEqual([
      expect.objectContaining({ type: "route.estimate", status: "skipped" }),
      expect.objectContaining({
        type: "travel.plan.generate",
        status: "failed",
        error: expect.objectContaining({ code: "MISSING_DESTINATION" })
      })
    ]);
    expect(route.calls).toHaveLength(0);
  });

  it("degrades outdoor reminders without origin and omits precise departure time", async () => {
    const route = new FakeRouteAdapter();
    const assistant = createTravelAssistant({
      route,
      profile: new FakeUserProfileAdapter({
        ok: true,
        profile: {}
      })
    });
    const response = await assistant.handleOutdoorEvent(outdoorTravelEvent());

    expect(response.speech).toContain("还不能给出精确出发时间");
    expect(response.context_patch.current_focus).not.toMatchObject({
      recommended_departure_time: expect.any(String)
    });
    expect(response.actions).toEqual([
      expect.objectContaining({ type: "user.profile.read", status: "success" }),
      expect.objectContaining({
        type: "route.estimate",
        status: "skipped",
        error: expect.objectContaining({ code: "MISSING_ORIGIN" })
      }),
      expect.objectContaining({ type: "travel.plan.generate", status: "success" })
    ]);
    expect(route.calls).toHaveLength(0);
  });

  it("keeps outdoor reminders useful when route lookup fails", async () => {
    const assistant = createTravelAssistant({
      route: new FakeRouteAdapter({
        ok: false,
        code: "ROUTE_TIMEOUT",
        message: "timeout"
      })
    });
    const response = await assistant.handleOutdoorEvent(outdoorTravelEvent());

    expect(response.speech).toContain("客户园区");
    expect(response.speech).toContain("还不能给出精确出发时间");
    expect(response.actions[1]).toMatchObject({
      type: "route.estimate",
      status: "failed",
      error: {
        code: "ROUTE_TIMEOUT"
      }
    });
  });

  it("generates business-trip weather and preparation reminders", async () => {
    const weather = new FakeWeatherAdapter();
    const assistant = createTravelAssistant({ weather });

    const response = await assistant.handleBusinessTripTomorrow(businessTripEvent());

    expect(response.speech).toContain("北京");
    expect(response.speech).toContain("北京明天多云");
    expect(response.speech).toContain("证件");
    expect(response.actions).toEqual([
      expect.objectContaining({
        type: "weather.forecast",
        status: "success",
        details: expect.objectContaining({
          destination: "北京",
          trigger_key: "trigger_trip"
        })
      }),
      expect.objectContaining({ type: "travel.plan.generate", status: "success" })
    ]);
    expect(weather.calls).toEqual([{ location: "北京", date: "2026-06-07" }]);
  });

  it("keeps business-trip preparation reminders when weather lookup fails", async () => {
    const weather = new FakeWeatherAdapter({
      ok: false,
      code: "WEATHER_TIMEOUT",
      message: "timeout"
    });
    const assistant = createTravelAssistant({ weather });

    const response = await assistant.handleBusinessTripTomorrow(businessTripEvent());

    expect(response.speech).toContain("暂时没有查到可用天气");
    expect(response.speech).toContain("证件");
    expect(response.actions[0]).toMatchObject({
      type: "weather.forecast",
      status: "failed",
      error: {
        code: "WEATHER_TIMEOUT"
      }
    });
    expect(response.actions[1]).toMatchObject({ type: "travel.plan.generate", status: "success" });
  });

  it("handles malformed scheduler travel events without planning adapter calls", async () => {
    const route = new FakeRouteAdapter();
    const weather = new FakeWeatherAdapter();
    const assistant = createTravelAssistant({ route, weather });

    const response = await assistant.handleOutdoorEvent({
      ...sampleEvent,
      type: "outdoor_event_detected",
      payload: {}
    });

    expect(response.actions).toEqual([
      expect.objectContaining({
        type: "travel.plan.generate",
        status: "failed",
        error: expect.objectContaining({ code: "MALFORMED_TRAVEL_EVENT" })
      })
    ]);
    expect(route.calls).toHaveLength(0);
    expect(weather.calls).toHaveLength(0);
  });
});

function createTravelAssistant(options: {
  route?: FakeRouteAdapter;
  weather?: FakeWeatherAdapter;
  profile?: FakeUserProfileAdapter;
} = {}) {
  return new TravelPlannerAssistant({
    routeAdapter: options.route ?? new FakeRouteAdapter(),
    weatherAdapter: options.weather ?? new FakeWeatherAdapter(),
    userProfileAdapter: options.profile ?? new FakeUserProfileAdapter()
  });
}

function outdoorTravelEvent(overrides: Partial<InputEvent> = {}): InputEvent {
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
    context: {
      timezone: "Asia/Shanghai"
    },
    ...overrides
  };
}

function businessTripEvent(overrides: Partial<InputEvent> = {}): InputEvent {
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
    context: {
      timezone: "Asia/Shanghai"
    },
    ...overrides
  };
}
