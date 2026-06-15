import type { WeatherCurrentRequest, WeatherForecastRequest, WeatherProviderConfig } from "./contracts.js";
export type WeatherProviderHandlerOptions = {
    config: WeatherProviderConfig;
    credential?: string;
    fetchImpl?: typeof fetch;
};
export declare function createWeatherProviderHandler(options: WeatherProviderHandlerOptions): {
    getCurrent(request: WeatherCurrentRequest): Promise<import("./contracts.js").WeatherCurrentResult>;
    getForecast(request: WeatherForecastRequest): Promise<import("./contracts.js").WeatherForecastResult>;
};
