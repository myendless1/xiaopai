import { describe, expect, it } from "vitest";
import {
  createDryRunUserProfileAdapter,
  DryRunRouteAdapter,
  DryRunWeatherAdapter,
  UnavailableRouteAdapter,
  UnavailableWeatherAdapter
} from "../src/travel/dry-run.js";

describe("travel dry-run adapters", () => {
  it("returns deterministic route estimates for known fixture destinations", async () => {
    const adapter = new DryRunRouteAdapter();
    const result = await adapter.estimateRoute({
      origin: "上海办公室",
      destination: "客户园区",
      departAt: "2026-06-06T18:00:00+08:00",
      mode: "driving"
    });

    expect(result).toMatchObject({
      ok: true,
      estimate: {
        durationMinutes: 42,
        distanceMeters: 18500,
        provider: "dry-run"
      }
    });
  });

  it("returns deterministic weather forecasts for known fixture destinations", async () => {
    const adapter = new DryRunWeatherAdapter();
    const result = await adapter.getForecast({
      location: "北京",
      date: "2026-06-07"
    });

    expect(result).toMatchObject({
      ok: true,
      forecast: {
        summary: expect.stringContaining("北京明天多云"),
        provider: "dry-run"
      }
    });
  });

  it("returns deterministic profile preferences without network calls", async () => {
    const adapter = createDryRunUserProfileAdapter({ arrivalBufferMinutes: 20 });
    const result = await adapter.readProfile({ userId: "ou_requester" });

    expect(result).toEqual({
      ok: true,
      profile: {
        originAddress: "上海办公室",
        defaultRouteMode: "driving",
        arrivalBufferMinutes: 20
      }
    });
  });

  it("uses structured failures for unavailable real providers", async () => {
    await expect(
      new UnavailableRouteAdapter().estimateRoute({
        origin: "上海办公室",
        destination: "客户园区",
        departAt: "2026-06-06T18:00:00+08:00",
        mode: "driving"
      })
    ).resolves.toMatchObject({ ok: false, code: "ROUTE_PROVIDER_NOT_CONFIGURED" });
    await expect(new UnavailableWeatherAdapter().getForecast({ location: "北京", date: "2026-06-07" })).resolves.toMatchObject({
      ok: false,
      code: "WEATHER_PROVIDER_NOT_CONFIGURED"
    });
  });
});
