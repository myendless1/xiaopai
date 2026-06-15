import { definePluginEntry, type OpenClawPluginApi } from "openclaw/plugin-sdk/plugin-entry";
import { readCredential, readPluginConfig } from "./config.js";
import type { WeatherCurrentRequest, WeatherForecastRequest } from "./contracts.js";
import { createWeatherProviderHandler } from "./handler.js";
import { createWeatherCurrentTool, createWeatherForecastTool } from "./tools.js";

type GatewayHandler = (args: { params: Record<string, unknown>; respond: (ok: boolean, payload: unknown) => void }) => Promise<void> | void;

export type WeatherProviderPluginEntry = {
  id: string;
  name: string;
  description: string;
  register(api: OpenClawPluginApi): void;
};

export function createDefaultWeatherProviderRuntime(api: { pluginConfig?: unknown }) {
  const config = readPluginConfig(api);
  const credential = readCredential(config);
  return {
    config,
    handler: createWeatherProviderHandler({
      config,
      ...(credential ? { credential } : {})
    })
  };
}

export const weatherProviderPlugin: WeatherProviderPluginEntry = definePluginEntry({
  id: "weather-provider",
  name: "Weather Provider",
  description: "Weather lookup tools and gateway methods backed by QWeather.",
  register(api: OpenClawPluginApi) {
    const runtime = createDefaultWeatherProviderRuntime(api);
    const handler = runtime.handler;
    registerGateway(api, "weather.getCurrent", async ({ params, respond }) => {
      respond(true, await handler.getCurrent(normalizeCurrentParams(params)));
    });
    registerGateway(api, "tool.weather.getCurrent", async ({ params, respond }) => {
      respond(true, await handler.getCurrent(normalizeCurrentParams(params)));
    });
    registerGateway(api, "weather.getForecast", async ({ params, respond }) => {
      respond(true, await handler.getForecast(normalizeForecastParams(params)));
    });
    registerGateway(api, "tool.weather.getForecast", async ({ params, respond }) => {
      respond(true, await handler.getForecast(normalizeForecastParams(params)));
    });
    api.registerTool(createWeatherCurrentTool({ config: runtime.config }));
    api.registerTool(createWeatherForecastTool({ config: runtime.config }));
  }
});

export default weatherProviderPlugin;
export { readCredential, readPluginConfig } from "./config.js";
export { createWeatherProviderHandler } from "./handler.js";
export { QWeatherClient } from "./qweather.js";
export * from "./contracts.js";

function registerGateway(api: OpenClawPluginApi, name: string, handler: GatewayHandler): void {
  api.registerGatewayMethod(name, handler, { scope: "operator.read" });
}

function normalizeCurrentParams(params: Record<string, unknown>): WeatherCurrentRequest {
  const request: WeatherCurrentRequest = {
    location: readRequiredString(params.location, "location")
  };
  const adm = readString(params.adm);
  const range = readString(params.range);
  if (adm) request.adm = adm;
  if (range) request.range = range;
  return request;
}

function normalizeForecastParams(params: Record<string, unknown>): WeatherForecastRequest {
  const request: WeatherForecastRequest = normalizeCurrentParams(params);
  const days = readForecastDays(params.days);
  const date = readString(params.date);
  if (days) request.days = days;
  if (date) request.date = date;
  return request;
}

function readRequiredString(value: unknown, field: string): string {
  const result = readString(value);
  if (!result) throw new Error(`${field} is required`);
  return result;
}

function readString(value: unknown): string | undefined {
  return typeof value === "string" && value.trim() !== "" ? value.trim() : undefined;
}

function readForecastDays(value: unknown): WeatherForecastRequest["days"] | undefined {
  return value === 3 || value === 7 || value === 10 || value === 15 || value === 30 ? value : undefined;
}
