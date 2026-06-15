import { describe, expect, it } from "vitest";
import { readCredential, readPluginConfig } from "../src/config.js";

describe("weather-provider config", () => {
  it("uses safe QWeather defaults without embedding credentials", () => {
    const config = readPluginConfig({ pluginConfig: {} });

    expect(config).toMatchObject({
      provider: "qweather",
      credentialEnv: "QWEATHER_API_KEY",
      authMode: "apiKeyHeader",
      defaultLanguage: "zh",
      defaultUnit: "metric",
      forecastDays: 3,
      timeoutMs: 5000
    });
    expect(config.apiHost).toBeUndefined();
  });

  it("reads credentials from the configured environment variable", () => {
    const config = readPluginConfig({
      pluginConfig: {
        credentialEnv: "QWEATHER_JWT",
        authMode: "jwtBearer"
      }
    });

    expect(readCredential(config, { QWEATHER_JWT: " token " })).toBe("token");
    expect(readCredential(config, {})).toBeUndefined();
  });
});
