import { definePluginEntry } from "openclaw/plugin-sdk/plugin-entry";
import { readCredential, readPluginConfig } from "./config.js";
import { createWeatherProviderHandler } from "./handler.js";
import { createWeatherCurrentTool, createWeatherForecastTool } from "./tools.js";
export function createDefaultWeatherProviderRuntime(api) {
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
export const weatherProviderPlugin = definePluginEntry({
    id: "weather-provider",
    name: "Weather Provider",
    description: "Weather lookup tools and gateway methods backed by QWeather.",
    register(api) {
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
function registerGateway(api, name, handler) {
    api.registerGatewayMethod(name, handler, { scope: "operator.read" });
}
function normalizeCurrentParams(params) {
    const request = {
        location: readRequiredString(params.location, "location")
    };
    const adm = readString(params.adm);
    const range = readString(params.range);
    if (adm)
        request.adm = adm;
    if (range)
        request.range = range;
    return request;
}
function normalizeForecastParams(params) {
    const request = normalizeCurrentParams(params);
    const days = readForecastDays(params.days);
    const date = readString(params.date);
    if (days)
        request.days = days;
    if (date)
        request.date = date;
    return request;
}
function readRequiredString(value, field) {
    const result = readString(value);
    if (!result)
        throw new Error(`${field} is required`);
    return result;
}
function readString(value) {
    return typeof value === "string" && value.trim() !== "" ? value.trim() : undefined;
}
function readForecastDays(value) {
    return value === 3 || value === 7 || value === 10 || value === 15 || value === 30 ? value : undefined;
}
//# sourceMappingURL=index.js.map