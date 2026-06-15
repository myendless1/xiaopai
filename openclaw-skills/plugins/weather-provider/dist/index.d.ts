import { type OpenClawPluginApi } from "openclaw/plugin-sdk/plugin-entry";
import type { WeatherCurrentRequest, WeatherForecastRequest } from "./contracts.js";
export type WeatherProviderPluginEntry = {
    id: string;
    name: string;
    description: string;
    register(api: OpenClawPluginApi): void;
};
export declare function createDefaultWeatherProviderRuntime(api: {
    pluginConfig?: unknown;
}): {
    config: import("./contracts.js").WeatherProviderConfig;
    handler: {
        getCurrent(request: WeatherCurrentRequest): Promise<import("./contracts.js").WeatherCurrentResult>;
        getForecast(request: WeatherForecastRequest): Promise<import("./contracts.js").WeatherForecastResult>;
    };
};
export declare const weatherProviderPlugin: WeatherProviderPluginEntry;
export default weatherProviderPlugin;
export { readCredential, readPluginConfig } from "./config.js";
export { createWeatherProviderHandler } from "./handler.js";
export { QWeatherClient } from "./qweather.js";
export * from "./contracts.js";
