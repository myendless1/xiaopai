import type { ForecastDays, WeatherAuthMode, WeatherProviderConfig, WeatherUnit } from "./contracts.js";
export declare const DEFAULT_CREDENTIAL_ENV = "QWEATHER_API_KEY";
export declare const DEFAULT_AUTH_MODE: WeatherAuthMode;
export declare const DEFAULT_LANGUAGE = "zh";
export declare const DEFAULT_UNIT: WeatherUnit;
export declare const DEFAULT_FORECAST_DAYS: ForecastDays;
export declare const DEFAULT_TIMEOUT_MS = 5000;
export declare function readPluginConfig(api: {
    pluginConfig?: unknown;
}): WeatherProviderConfig;
export declare function readCredential(config: WeatherProviderConfig, env?: NodeJS.ProcessEnv): string | undefined;
export declare function normalizeApiHost(value: string | undefined): string | undefined;
export declare function readForecastDays(value: unknown): ForecastDays | undefined;
