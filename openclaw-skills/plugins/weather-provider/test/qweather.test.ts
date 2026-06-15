import { describe, expect, it, vi } from "vitest";
import { readPluginConfig } from "../src/config.js";
import { QWeatherClient } from "../src/qweather.js";

describe("QWeatherClient", () => {
  it("looks up location and returns selected daily forecast", async () => {
    const calls: Array<{ url: URL; headers: Headers }> = [];
    const fetchImpl = vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
      const url = new URL(String(input));
      calls.push({ url, headers: new Headers(init?.headers) });
      if (url.pathname === "/geo/v2/city/lookup") {
        return jsonResponse({
          code: "200",
          location: [
            {
              id: "101010100",
              name: "北京",
              country: "中国",
              adm1: "北京市",
              adm2: "北京",
              lat: "39.90",
              lon: "116.40",
              tz: "Asia/Shanghai"
            }
          ]
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
            precipProbability: "20",
            humidity: "55",
            windDirDay: "东北风",
            windScaleDay: "1-3"
          }
        ]
      });
    });
    const client = new QWeatherClient({
      config: readPluginConfig({
        pluginConfig: {
          apiHost: "https://unit-test.qweather.example",
          defaultRange: "cn"
        }
      }),
      credential: "test-key",
      fetchImpl
    });

    const result = await client.getForecast({ location: "北京", date: "2026-06-15" });

    expect(result).toMatchObject({
      ok: true,
      selected: {
        date: "2026-06-15",
        summary: expect.stringContaining("北京2026-06-15天气多云转晴"),
        lowCelsius: 18,
        highCelsius: 27,
        precipitationChance: 20,
        provider: "qweather"
      }
    });
    expect(calls).toHaveLength(2);
    expect(calls[0]?.url.pathname).toBe("/geo/v2/city/lookup");
    expect(calls[0]?.url.searchParams.get("location")).toBe("北京");
    expect(calls[0]?.url.searchParams.get("range")).toBe("cn");
    expect(calls[0]?.headers.get("X-QW-Api-Key")).toBe("test-key");
    expect(calls[1]?.url.pathname).toBe("/v7/weather/3d");
  });

  it("returns a structured failure when credentials are missing", async () => {
    const client = new QWeatherClient({
      config: readPluginConfig({
        pluginConfig: {
          apiHost: "https://unit-test.qweather.example",
          credentialEnv: "QWEATHER_API_KEY"
        }
      })
    });

    await expect(client.getCurrent({ location: "北京" })).resolves.toEqual({
      ok: false,
      code: "QWEATHER_CREDENTIAL_MISSING",
      message: "QWeather credential environment variable QWEATHER_API_KEY is not set.",
      provider: "qweather"
    });
  });

  it("supports JWT bearer authentication", async () => {
    const fetchImpl = vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
      const url = new URL(String(input));
      if (url.pathname === "/geo/v2/city/lookup") {
        return jsonResponse({ code: "200", location: [{ id: "101020100", name: "上海" }] });
      }
      return jsonResponse({ code: "200", now: { obsTime: "2026-06-14T12:00+08:00", temp: "31", text: "晴", feelsLike: "33" } });
    });
    const client = new QWeatherClient({
      config: readPluginConfig({
        pluginConfig: {
          apiHost: "https://unit-test.qweather.example",
          authMode: "jwtBearer",
          credentialEnv: "QWEATHER_JWT"
        }
      }),
      credential: "jwt-token",
      fetchImpl
    });

    const result = await client.getCurrent({ location: "上海" });

    expect(result).toMatchObject({
      ok: true,
      current: {
        summary: expect.stringContaining("上海当前晴")
      }
    });
    expect(fetchImpl.mock.calls[0]?.[1]?.headers).toEqual(expect.any(Headers));
    expect(new Headers(fetchImpl.mock.calls[0]?.[1]?.headers).get("Authorization")).toBe("Bearer jwt-token");
  });
});

function jsonResponse(value: unknown): Response {
  return new Response(JSON.stringify(value), {
    status: 200,
    headers: {
      "content-type": "application/json"
    }
  });
}
