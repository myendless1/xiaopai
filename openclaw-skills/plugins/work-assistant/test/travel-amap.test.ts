import { describe, expect, it, vi } from "vitest";
import { createDefaultWorkAssistantRuntime } from "../src/index.js";
import { AmapRouteAdapter } from "../src/travel/amap-route.js";
import { sampleEvent } from "./helpers.js";

describe("Amap travel route adapter", () => {
  it("returns a route estimate from geocoded driving route data", async () => {
    const calls: URL[] = [];
    const fetchImpl = vi.fn(async (input: RequestInfo | URL) => {
      const url = new URL(String(input));
      calls.push(url);
      if (url.pathname === "/v3/geocode/geo" && url.searchParams.get("address") === "上海办公室") {
        return jsonResponse({
          status: "1",
          info: "OK",
          infocode: "10000",
          count: "1",
          geocodes: [{ formatted_address: "上海市黄浦区人民广场", location: "121.473667,31.230525" }]
        });
      }
      if (url.pathname === "/v3/geocode/geo" && url.searchParams.get("address") === "上海虹桥火车站") {
        return jsonResponse({
          status: "1",
          info: "OK",
          infocode: "10000",
          count: "1",
          geocodes: [{ formatted_address: "上海虹桥站", location: "121.327538,31.200299" }]
        });
      }
      return jsonResponse({
        status: "1",
        info: "OK",
        infocode: "10000",
        route: {
          paths: [{ distance: "17800", duration: "2380" }]
        }
      });
    });
    const adapter = new AmapRouteAdapter({
      config: {
        provider: "amap",
        defaultCity: "上海"
      },
      credential: "test-key",
      fetchImpl
    });

    const result = await adapter.estimateRoute({
      origin: "上海办公室",
      destination: "上海虹桥火车站",
      departAt: "2026-06-18T15:00:00+08:00",
      mode: "driving"
    });

    expect(result).toEqual({
      ok: true,
      estimate: {
        origin: "上海办公室",
        destination: "上海虹桥火车站",
        durationMinutes: 40,
        distanceMeters: 17800,
        mode: "driving",
        provider: "amap"
      }
    });
    expect(calls.map((url) => url.pathname)).toEqual([
      "/v3/geocode/geo",
      "/v3/geocode/geo",
      "/v3/direction/driving"
    ]);
    expect(calls[0]?.searchParams.get("city")).toBe("上海");
    expect(calls[0]?.searchParams.get("key")).toBe("test-key");
  });

  it("falls back to place search when geocoding does not find a destination", async () => {
    const calls: URL[] = [];
    const fetchImpl = vi.fn(async (input: RequestInfo | URL) => {
      const url = new URL(String(input));
      calls.push(url);
      if (url.pathname === "/v3/geocode/geo") {
        return jsonResponse({
          status: "1",
          info: "OK",
          infocode: "10000",
          count: "0",
          geocodes: []
        });
      }
      if (url.pathname === "/v3/place/text") {
        return jsonResponse({
          status: "1",
          info: "OK",
          infocode: "10000",
          count: "1",
          pois: [{ name: "客户园区", address: "浦东新区", location: "121.602725,31.204897" }]
        });
      }
      return jsonResponse({
        status: "1",
        info: "OK",
        infocode: "10000",
        route: {
          paths: [{ distance: "920", duration: "780" }]
        }
      });
    });
    const adapter = new AmapRouteAdapter({
      config: {
        provider: "amap",
        defaultCity: "上海"
      },
      credential: "test-key",
      fetchImpl
    });

    const result = await adapter.estimateRoute({
      origin: "121.473667,31.230525",
      destination: "客户园区",
      departAt: "2026-06-18T15:00:00+08:00",
      mode: "walking"
    });

    expect(result).toMatchObject({
      ok: true,
      estimate: {
        durationMinutes: 13,
        distanceMeters: 920,
        mode: "walking",
        provider: "amap"
      }
    });
    expect(calls.map((url) => url.pathname)).toEqual([
      "/v3/geocode/geo",
      "/v3/place/text",
      "/v3/direction/walking"
    ]);
    expect(calls[1]?.searchParams.get("citylimit")).toBe("true");
  });

  it("degrades outdoor reminders when Amap credentials are not configured", async () => {
    const previous = process.env.AMAP_MISSING_TEST_KEY;
    delete process.env.AMAP_MISSING_TEST_KEY;
    try {
      const runtime = createDefaultWorkAssistantRuntime({
        pluginConfig: {
          dryRun: false,
          travel: {
            originAddress: "121.473667,31.230525",
            route: {
              provider: "amap",
              credentialEnv: "AMAP_MISSING_TEST_KEY",
              defaultCity: "上海"
            }
          },
          scheduler: {
            enabled: false
          }
        }
      } as never);

      const response = await runtime.assistant.handleEvent(outdoorTravelEvent());

      expect(response.speech).toContain("还不能给出精确出发时间");
      expect(response.actions[1]).toMatchObject({
        type: "route.estimate",
        status: "failed",
        error: {
          code: "AMAP_CREDENTIAL_MISSING"
        }
      });
      expect(response.actions[2]).toMatchObject({ type: "travel.plan.generate", status: "success" });
    } finally {
      if (previous === undefined) {
        delete process.env.AMAP_MISSING_TEST_KEY;
      } else {
        process.env.AMAP_MISSING_TEST_KEY = previous;
      }
    }
  });
});

function outdoorTravelEvent() {
  return {
    ...sampleEvent,
    event_id: "evt-outdoor-travel-amap",
    type: "outdoor_event_detected",
    timestamp: "2026-06-18T14:00:00+08:00",
    payload: {
      trigger: {
        rule_id: "outdoor_event",
        scheduled_for: "2026-06-18T06:00:00.000Z",
        fired_at: "2026-06-18T06:00:00.000Z",
        source: "proactive_calendar_scheduler",
        trigger_key: "trigger_outdoor_amap"
      },
      calendar_event: {
        id: "outdoor_amap_1",
        title: "外出上海虹桥火车站拜访",
        start: "2026-06-18T15:00:00+08:00",
        end: "2026-06-18T16:00:00+08:00",
        location: "上海虹桥火车站"
      }
    },
    context: {
      timezone: "Asia/Shanghai"
    }
  };
}

function jsonResponse(value: unknown): Response {
  return new Response(JSON.stringify(value), {
    status: 200,
    headers: {
      "content-type": "application/json"
    }
  });
}
