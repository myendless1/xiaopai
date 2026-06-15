export type WeatherProviderName = "qweather";
export type WeatherAuthMode = "apiKeyHeader" | "apiKeyQuery" | "jwtBearer";
export type WeatherUnit = "metric" | "imperial";
export type ForecastDays = 3 | 7 | 10 | 15 | 30;
export type WeatherCondition = "clear" | "cloudy" | "rain" | "snow" | "wind" | "hot" | "cold";
export type WeatherProviderConfig = {
    provider: WeatherProviderName;
    apiHost?: string;
    credentialEnv: string;
    authMode: WeatherAuthMode;
    defaultLanguage: string;
    defaultUnit: WeatherUnit;
    defaultRange?: string;
    forecastDays: ForecastDays;
    timeoutMs: number;
};
export type WeatherLocation = {
    id: string;
    name: string;
    country?: string;
    adm1?: string;
    adm2?: string;
    latitude?: number;
    longitude?: number;
    timezone?: string;
    link?: string;
};
export type WeatherCurrentRequest = {
    location: string;
    adm?: string;
    range?: string;
    language?: string;
    unit?: WeatherUnit;
};
export type WeatherForecastRequest = WeatherCurrentRequest & {
    days?: ForecastDays;
    date?: string;
};
export type WeatherCurrent = {
    provider: WeatherProviderName;
    location: WeatherLocation;
    observedAt: string;
    summary: string;
    condition?: WeatherCondition;
    tempCelsius?: number;
    feelsLikeCelsius?: number;
    humidityPercent?: number;
    windDirection?: string;
    windScale?: string;
    windSpeedKph?: number;
};
export type WeatherDailyForecast = {
    provider: WeatherProviderName;
    location: WeatherLocation;
    date: string;
    summary: string;
    condition?: WeatherCondition;
    lowCelsius?: number;
    highCelsius?: number;
    precipitationChance?: number;
    humidityPercent?: number;
    windDirection?: string;
    windScale?: string;
};
export type WeatherCurrentResult = {
    ok: true;
    current: WeatherCurrent;
} | WeatherFailure;
export type WeatherForecastResult = {
    ok: true;
    forecast: WeatherDailyForecast[];
    selected?: WeatherDailyForecast;
    summary: string;
} | WeatherFailure;
export type WeatherFailure = {
    ok: false;
    code: string;
    message: string;
    provider?: WeatherProviderName;
    details?: Record<string, unknown>;
};
