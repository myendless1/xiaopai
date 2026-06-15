export const DEFAULT_CREDENTIAL_ENV = "QWEATHER_API_KEY";
export const DEFAULT_AUTH_MODE = "apiKeyHeader";
export const DEFAULT_LANGUAGE = "zh";
export const DEFAULT_UNIT = "metric";
export const DEFAULT_FORECAST_DAYS = 3;
export const DEFAULT_TIMEOUT_MS = 5000;
export function readPluginConfig(api) {
    const raw = isRecord(api.pluginConfig) ? api.pluginConfig : {};
    const config = {
        provider: "qweather",
        credentialEnv: readNonEmptyString(raw.credentialEnv) ?? DEFAULT_CREDENTIAL_ENV,
        authMode: readAuthMode(raw.authMode) ?? DEFAULT_AUTH_MODE,
        defaultLanguage: readNonEmptyString(raw.defaultLanguage) ?? DEFAULT_LANGUAGE,
        defaultUnit: readUnit(raw.defaultUnit) ?? DEFAULT_UNIT,
        forecastDays: readForecastDays(raw.forecastDays) ?? DEFAULT_FORECAST_DAYS,
        timeoutMs: readPositiveNumber(raw.timeoutMs) ?? DEFAULT_TIMEOUT_MS
    };
    const apiHost = normalizeApiHost(readNonEmptyString(raw.apiHost));
    const defaultRange = readNonEmptyString(raw.defaultRange);
    if (apiHost)
        config.apiHost = apiHost;
    if (defaultRange)
        config.defaultRange = defaultRange;
    return config;
}
export function readCredential(config, env = process.env) {
    const value = env[config.credentialEnv];
    return typeof value === "string" && value.trim() !== "" ? value.trim() : undefined;
}
export function normalizeApiHost(value) {
    if (!value)
        return undefined;
    const trimmed = value.trim().replace(/\/+$/g, "");
    return /^https?:\/\//.test(trimmed) ? trimmed : undefined;
}
export function readForecastDays(value) {
    return value === 3 || value === 7 || value === 10 || value === 15 || value === 30 ? value : undefined;
}
function readAuthMode(value) {
    return value === "apiKeyHeader" || value === "apiKeyQuery" || value === "jwtBearer" ? value : undefined;
}
function readUnit(value) {
    return value === "metric" || value === "imperial" ? value : undefined;
}
function readPositiveNumber(value) {
    return typeof value === "number" && Number.isFinite(value) && value > 0 ? value : undefined;
}
function readNonEmptyString(value) {
    return typeof value === "string" && value.trim() !== "" ? value.trim() : undefined;
}
function isRecord(value) {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}
//# sourceMappingURL=config.js.map