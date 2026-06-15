import type { ForecastDays, WeatherAuthMode, WeatherCurrentRequest, WeatherCurrentResult, WeatherForecastRequest, WeatherForecastResult, WeatherLocation, WeatherProviderConfig } from "./contracts.js";
export type QWeatherClientOptions = {
    config: WeatherProviderConfig;
    credential?: string;
    fetchImpl?: typeof fetch;
};
export declare class QWeatherClient {
    private readonly config;
    private readonly credential;
    private readonly fetchImpl;
    constructor(options: QWeatherClientOptions);
    getCurrent(request: WeatherCurrentRequest): Promise<WeatherCurrentResult>;
    getForecast(request: WeatherForecastRequest): Promise<WeatherForecastResult>;
    lookupLocation(request: Pick<WeatherCurrentRequest, "location" | "adm" | "range" | "language">): Promise<{
        ok: true;
        location: WeatherLocation;
    } | {
        ok: false;
        code: string;
        message: string;
        provider: "qweather";
        details?: Record<string, unknown>;
    }>;
    private requestJson;
    private checkConfigured;
    private applyAuth;
}
export declare function isForecastDays(value: unknown): value is ForecastDays;
export declare function isAuthMode(value: unknown): value is WeatherAuthMode;
