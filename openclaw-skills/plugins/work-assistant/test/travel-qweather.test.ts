import { describe, expect, it, vi } from "vitest";
import { createDefaultWorkAssistantRuntime } from "../src/index.js";
import { QWeatherWeatherAdapter } from "../src/travel/qweather.js";
import { sampleEvent } from "./helpers.js";

describe("QWeather travel adapter", () => {
  it("returns a work-assistant weather forecast from QWeather daily data", async () => {
    const calls: Array<{ url: URL; headers: Headers }> = [];
    const fetchImpl = vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
      const url = new URL(String(input));
      calls.push({ url, headers: new Headers(init?.headers) });
      if (url.pathname === "/geo/v2/city/lookup") {
        return jsonResponse({
          code: "200",
          location: [{ id: "101010100", name: "北京" }]
        });
      }
      return jsonResponse({
        code: "200",
        daily: [
          {
            fxDate: "2026-06-15",
            tempMax: "27",
            tempMin: "18",
            textDay: "多云",
            textNight: "晴",
            precipProbability: "20"
          }
        ]
      });
    });
    const adapter = new QWeatherWeatherAdapter({
      config: {
        provider: "qweather",
        apiHost: "https://unit-test.qweather.example",
        defaultRange: "cn"
      },
      credential: "test-key",
      fetchImpl
    });

    const result = await adapter.getForecast({ location: "北京", date: "2026-06-15" });

    expect(result).toMatchObject({
      ok: true,
      forecast: {
        location: "北京",
        date: "2026-06-15",
        summary: expect.stringContaining("北京2026-06-15天气多云转晴"),
        lowCelsius: 18,
        highCelsius: 27,
        precipitationChance: 20,
        provider: "qweather"
      }
    });
    expect(calls[0]?.url.pathname).toBe("/geo/v2/city/lookup");
    expect(calls[0]?.url.searchParams.get("range")).toBe("cn");
    expect(calls[0]?.headers.get("X-QW-Api-Key")).toBe("test-key");
    expect(calls[1]?.url.pathname).toBe("/v7/weather/3d");
  });

  it("degrades business trip reminders when QWeather credentials are not configured", async () => {
    const runtime = createDefaultWorkAssistantRuntime({
      pluginConfig: {
        dryRun: false,
        travel: {
          weather: {
            provider: "qweather",
            apiHost: "https://unit-test.qweather.example",
            credentialEnv: "QWEATHER_MISSING_TEST_KEY"
          }
        },
        scheduler: {
          enabled: false
        }
      }
    } as never);

    const response = await runtime.assistant.handleEvent(businessTripEvent());

    expect(response.speech).toContain("暂时没有查到可用天气");
    expect(response.actions[0]).toMatchObject({
      type: "weather.forecast",
      status: "failed",
      error: {
        code: "QWEATHER_CREDENTIAL_MISSING"
      }
    });
    expect(response.actions[1]).toMatchObject({ type: "travel.plan.generate", status: "success" });
  });
});

function businessTripEvent() {
  return {
    ...sampleEvent,
    event_id: "evt-business-trip-qweather",
    type: "business_trip_tomorrow_detected",
    timestamp: "2026-06-14T18:00:00+08:00",
    payload: {
      trigger: {
        rule_id: "business_trip_tomorrow",
        scheduled_for: "2026-06-14T10:00:00.000Z",
        fired_at: "2026-06-14T10:00:00.000Z",
        source: "proactive_calendar_scheduler",
        trigger_key: "trigger_trip_qweather"
      },
      calendar_event: {
        id: "trip_1",
        title: "北京出差",
        start: "2026-06-15T10:00:00+08:00",
        end: "2026-06-15T18:00:00+08:00"
      }
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
