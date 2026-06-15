import { describe, expect, it } from "vitest";
import weatherProviderPlugin from "../src/index.js";

type GatewayHandler = (args: { params: Record<string, unknown>; respond: (ok: boolean, payload: unknown) => void }) => Promise<void> | void;

describe("weather-provider plugin entry", () => {
  it("registers gateway methods and agent tools", async () => {
    const methods = new Map<string, GatewayHandler>();
    const tools: Array<{ name: string }> = [];

    weatherProviderPlugin.register({
      pluginConfig: {
        apiHost: "https://unit-test.qweather.example",
        credentialEnv: "QWEATHER_API_KEY"
      },
      registerGatewayMethod(name: string, handler: GatewayHandler) {
        methods.set(name, handler);
      },
      registerTool(tool: { name: string }) {
        tools.push(tool);
      }
    } as never);

    expect([...methods.keys()].sort()).toEqual([
      "tool.weather.getCurrent",
      "tool.weather.getForecast",
      "weather.getCurrent",
      "weather.getForecast"
    ]);
    expect(tools.map((tool) => tool.name).sort()).toEqual(["weather_current", "weather_forecast"]);

    let payload: unknown;
    await methods.get("weather.getCurrent")?.({
      params: { location: "北京" },
      respond(ok, response) {
        expect(ok).toBe(true);
        payload = response;
      }
    });
    expect(payload).toMatchObject({
      ok: false,
      code: "QWEATHER_CREDENTIAL_MISSING"
    });
  });
});
