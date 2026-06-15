import type { QWeatherAuthMode, QWeatherForecastDays, QWeatherUnit, QWeatherWeatherConfig, WeatherAdapter, WeatherForecastRequest, WeatherForecastResult } from "./adapters.js";
type QWeatherAdapterOptions = {
    config: QWeatherWeatherConfig;
    credential?: string;
    fetchImpl?: typeof fetch;
};
export declare class QWeatherWeatherAdapter implements WeatherAdapter {
    private readonly config;
    private readonly credential;
    private readonly fetchImpl;
    constructor(options: QWeatherAdapterOptions);
    getForecast(request: WeatherForecastRequest): Promise<WeatherForecastResult>;
    private lookupLocation;
    private fetchDaily;
    private requestJson;
    private applyAuth;
    private checkConfigured;
}
type RequiredQWeatherConfig = {
    provider: "qweather";
    apiHost?: string;
    credentialEnv: string;
    authMode: QWeatherAuthMode;
    defaultLanguage: string;
    defaultUnit: QWeatherUnit;
    defaultRange?: string;
    forecastDays: QWeatherForecastDays;
    timeoutMs: number;
};
export declare function normalizeQWeatherConfig(config: QWeatherWeatherConfig): RequiredQWeatherConfig;
export declare function readForecastDays(value: unknown): QWeatherForecastDays | undefined;
export {};
