import type {
  QWeatherAuthMode,
  QWeatherForecastDays,
  QWeatherUnit,
  QWeatherWeatherConfig,
  WeatherAdapter,
  WeatherForecast,
  WeatherForecastRequest,
  WeatherForecastResult
} from "./adapters.js";

type QWeatherLocation = {
  id?: string;
  name?: string;
};

type QWeatherGeoResponse = {
  code?: string;
  location?: QWeatherLocation[];
};

type QWeatherDailyResponse = {
  code?: string;
  daily?: Array<{
    fxDate?: string;
    tempMax?: string;
    tempMin?: string;
    textDay?: string;
    textNight?: string;
    humidity?: string;
    windDirDay?: string;
    windScaleDay?: string;
    precipProbability?: string;
  }>;
};

type QWeatherAdapterOptions = {
  config: QWeatherWeatherConfig;
  credential?: string;
  fetchImpl?: typeof fetch;
};

type QWeatherError = {
  code: string;
  message: string;
};

const DEFAULT_CREDENTIAL_ENV = "QWEATHER_API_KEY";
const DEFAULT_AUTH_MODE: QWeatherAuthMode = "apiKeyHeader";
const DEFAULT_LANGUAGE = "zh";
const DEFAULT_UNIT: QWeatherUnit = "metric";
const DEFAULT_FORECAST_DAYS: QWeatherForecastDays = 3;
const DEFAULT_TIMEOUT_MS = 5000;

export class QWeatherWeatherAdapter implements WeatherAdapter {
  private readonly config: RequiredQWeatherConfig;
  private readonly credential: string | undefined;
  private readonly fetchImpl: typeof fetch;

  constructor(options: QWeatherAdapterOptions) {
    this.config = normalizeQWeatherConfig(options.config);
    this.credential = options.credential ?? readCredential(this.config);
    this.fetchImpl = options.fetchImpl ?? fetch;
  }

  async getForecast(request: WeatherForecastRequest): Promise<WeatherForecastResult> {
    const configured = this.checkConfigured();
    if (configured) return configured;
    const location = await this.lookupLocation(request.location);
    if (!location.ok) return location;
    const daily = await this.fetchDaily(location.id);
    if (!daily.ok) return daily;
    const matched = daily.items.find((item) => item.fxDate === request.date);
    if (!matched) {
      return {
        ok: false,
        code: "QWEATHER_FORECAST_DATE_UNAVAILABLE",
        message: `QWeather forecast did not include ${request.date}.`
      };
    }
    return {
      ok: true,
      forecast: normalizeForecast(location.name, request.date, matched)
    };
  }

  private async lookupLocation(location: string): Promise<
    | {
        ok: true;
        id: string;
        name: string;
      }
    | {
        ok: false;
        code: string;
        message: string;
      }
  > {
    const direct = resolveDirectLocation(location);
    if (direct) return { ok: true, id: direct.id, name: direct.name };
    const known = resolveKnownLocation(location);
    const response = await this.requestJson<QWeatherGeoResponse>("/geo/v2/city/lookup", {
      location,
      range: this.config.defaultRange,
      number: "1",
      lang: this.config.defaultLanguage
    });
    if (!response.ok) {
      if (known) return { ok: true, id: known.id, name: known.name };
      return response;
    }
    if (response.data.code !== "200") {
      if (known) return { ok: true, id: known.id, name: known.name };
      return qweatherCodeFailure(response.data.code, "QWeather location lookup failed.");
    }
    const first = response.data.location?.[0];
    if ((!first?.id || !first.name) && known) return { ok: true, id: known.id, name: known.name };
    if (!first?.id || !first.name) {
      return {
        ok: false,
        code: "QWEATHER_LOCATION_NOT_FOUND",
        message: `No QWeather location matched "${location}".`
      };
    }
    return {
      ok: true,
      id: first.id,
      name: first.name
    };
  }

  private async fetchDaily(locationId: string): Promise<
    | {
        ok: true;
        items: NonNullable<QWeatherDailyResponse["daily"]>;
      }
    | {
        ok: false;
        code: string;
        message: string;
      }
  > {
    const response = await this.requestJson<QWeatherDailyResponse>(`/v7/weather/${this.config.forecastDays}d`, {
      location: locationId,
      lang: this.config.defaultLanguage,
      unit: qweatherUnit(this.config.defaultUnit)
    });
    if (!response.ok) return response;
    if (response.data.code !== "200") return qweatherCodeFailure(response.data.code, "QWeather daily forecast lookup failed.");
    if (!Array.isArray(response.data.daily) || response.data.daily.length === 0) {
      return {
        ok: false,
        code: "QWEATHER_MALFORMED_RESPONSE",
        message: "QWeather response did not include daily forecast data."
      };
    }
    return {
      ok: true,
      items: response.data.daily
    };
  }

  private async requestJson<T>(
    path: string,
    params: Record<string, string | undefined>
  ): Promise<
    | {
        ok: true;
        data: T;
      }
    | {
        ok: false;
        code: string;
        message: string;
      }
  > {
    const url = new URL(`${this.config.apiHost}${path}`);
    for (const [key, value] of Object.entries(params)) {
      if (value !== undefined && value !== "") url.searchParams.set(key, value);
    }
    const headers = new Headers({ Accept: "application/json" });
    this.applyAuth(url, headers);
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.config.timeoutMs);
    try {
      const response = await this.fetchImpl(url, {
        headers,
        signal: controller.signal
      });
      const text = await response.text();
      if (!response.ok) {
        return {
          ok: false,
          code: "QWEATHER_HTTP_ERROR",
          message: `QWeather HTTP request failed with status ${response.status}: ${text.slice(0, 120)}`
        };
      }
      return {
        ok: true,
        data: JSON.parse(text) as T
      };
    } catch (error) {
      return {
        ok: false,
        code: readErrorName(error) === "AbortError" ? "QWEATHER_TIMEOUT" : "QWEATHER_REQUEST_ERROR",
        message: safeErrorMessage(error)
      };
    } finally {
      clearTimeout(timer);
    }
  }

  private applyAuth(url: URL, headers: Headers): void {
    const credential = this.credential ?? "";
    if (this.config.authMode === "jwtBearer") {
      headers.set("Authorization", `Bearer ${credential}`);
      return;
    }
    if (this.config.authMode === "apiKeyQuery") {
      url.searchParams.set("key", credential);
      return;
    }
    headers.set("X-QW-Api-Key", credential);
  }

  private checkConfigured(): WeatherForecastResult | undefined {
    if (!this.config.apiHost) {
      return {
        ok: false,
        code: "QWEATHER_NOT_CONFIGURED",
        message: "QWeather apiHost is not configured."
      };
    }
    if (!this.credential) {
      return {
        ok: false,
        code: "QWEATHER_CREDENTIAL_MISSING",
        message: `QWeather credential environment variable ${this.config.credentialEnv} is not set.`
      };
    }
    return undefined;
  }
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

export function normalizeQWeatherConfig(config: QWeatherWeatherConfig): RequiredQWeatherConfig {
  const normalized: RequiredQWeatherConfig = {
    provider: "qweather",
    credentialEnv: readNonEmptyString(config.credentialEnv) ?? DEFAULT_CREDENTIAL_ENV,
    authMode: readAuthMode(config.authMode) ?? DEFAULT_AUTH_MODE,
    defaultLanguage: readNonEmptyString(config.defaultLanguage) ?? DEFAULT_LANGUAGE,
    defaultUnit: readUnit(config.defaultUnit) ?? DEFAULT_UNIT,
    forecastDays: readForecastDays(config.forecastDays) ?? DEFAULT_FORECAST_DAYS,
    timeoutMs: readPositiveNumber(config.timeoutMs) ?? DEFAULT_TIMEOUT_MS
  };
  const apiHost = normalizeApiHost(config.apiHost);
  const defaultRange = readNonEmptyString(config.defaultRange);
  if (apiHost) normalized.apiHost = apiHost;
  if (defaultRange) normalized.defaultRange = defaultRange;
  return normalized;
}

function normalizeForecast(location: string, date: string, item: NonNullable<QWeatherDailyResponse["daily"]>[number]): WeatherForecast {
  const low = readNumber(item.tempMin);
  const high = readNumber(item.tempMax);
  const precipitationChance = readNumber(item.precipProbability);
  const text = normalizeDayNightText(item.textDay, item.textNight);
  const forecast: WeatherForecast = {
    location,
    date,
    summary: buildForecastSummary(location, date, text, low, high, precipitationChance),
    provider: "qweather"
  };
  const condition = inferCondition(text, high, low);
  if (condition) forecast.condition = condition;
  if (low !== undefined) forecast.lowCelsius = low;
  if (high !== undefined) forecast.highCelsius = high;
  if (precipitationChance !== undefined) forecast.precipitationChance = precipitationChance;
  return forecast;
}

function resolveDirectLocation(value: string): { id: string; name: string } | undefined {
  const trimmed = value.trim();
  if (!/^\d{6,12}$/.test(trimmed)) return undefined;
  return {
    id: trimmed,
    name: trimmed
  };
}

function resolveKnownLocation(value: string): { id: string; name: string } | undefined {
  const key = normalizeLocationKey(value);
  return KNOWN_CHINA_LOCATION_IDS.find((item) => item.aliases.some((alias) => normalizeLocationKey(alias) === key));
}

const KNOWN_CHINA_LOCATION_IDS: Array<{ id: string; name: string; aliases: string[] }> = [
  { id: "101010100", name: "北京", aliases: ["北京", "北京市", "Beijing"] },
  { id: "101020100", name: "上海", aliases: ["上海", "上海市", "Shanghai"] },
  { id: "101030100", name: "天津", aliases: ["天津", "天津市", "Tianjin"] },
  { id: "101040100", name: "重庆", aliases: ["重庆", "重庆市", "Chongqing"] },
  { id: "101280101", name: "广州", aliases: ["广州", "广州市", "Guangzhou"] },
  { id: "101280601", name: "深圳", aliases: ["深圳", "深圳市", "Shenzhen"] },
  { id: "101210101", name: "杭州", aliases: ["杭州", "杭州市", "Hangzhou"] },
  { id: "101190101", name: "南京", aliases: ["南京", "南京市", "Nanjing"] },
  { id: "101270101", name: "成都", aliases: ["成都", "成都市", "Chengdu"] },
  { id: "101200101", name: "武汉", aliases: ["武汉", "武汉市", "Wuhan"] },
  { id: "101110101", name: "西安", aliases: ["西安", "西安市", "Xian", "Xi'an"] },
  { id: "101250101", name: "长沙", aliases: ["长沙", "长沙市", "Changsha"] },
  { id: "101180101", name: "郑州", aliases: ["郑州", "郑州市", "Zhengzhou"] },
  { id: "101190401", name: "苏州", aliases: ["苏州", "苏州市", "Suzhou"] },
  { id: "101120201", name: "青岛", aliases: ["青岛", "青岛市", "Qingdao"] },
  { id: "101120101", name: "济南", aliases: ["济南", "济南市", "Jinan"] },
  { id: "101230201", name: "厦门", aliases: ["厦门", "厦门市", "Xiamen"] },
  { id: "101230101", name: "福州", aliases: ["福州", "福州市", "Fuzhou"] },
  { id: "101220101", name: "合肥", aliases: ["合肥", "合肥市", "Hefei"] },
  { id: "101070101", name: "沈阳", aliases: ["沈阳", "沈阳市", "Shenyang"] },
  { id: "101070201", name: "大连", aliases: ["大连", "大连市", "Dalian"] },
  { id: "101210401", name: "宁波", aliases: ["宁波", "宁波市", "Ningbo"] },
  { id: "101190201", name: "无锡", aliases: ["无锡", "无锡市", "Wuxi"] }
];

function normalizeLocationKey(value: string): string {
  return value.trim().toLocaleLowerCase().replace(/\s+/g, "").replace(/市$/u, "");
}

function buildForecastSummary(location: string, date: string, text: string, low: number | undefined, high: number | undefined, precipitationChance: number | undefined): string {
  const parts = [`${location}${date}天气${text}`];
  if (low !== undefined && high !== undefined) parts.push(`${formatNumber(low)}到${formatNumber(high)}度`);
  if (precipitationChance !== undefined) parts.push(`降水概率${formatNumber(precipitationChance)}%`);
  return `${parts.join("，")}。`;
}

function normalizeDayNightText(day: string | undefined, night: string | undefined): string {
  if (day && night && day !== night) return `${day}转${night}`;
  return day ?? night ?? "天气状况未知";
}

function inferCondition(
  text: string,
  high: number | undefined,
  low: number | undefined
): "clear" | "cloudy" | "rain" | "snow" | "wind" | "hot" | "cold" | undefined {
  if (/雪|霰|冰雹/.test(text)) return "snow";
  if (/雨|雷|阵雨|暴雨/.test(text)) return "rain";
  if (/风|沙|尘|扬沙|浮尘/.test(text)) return "wind";
  if ((high ?? low ?? 0) >= 32) return "hot";
  if ((low ?? high ?? 99) <= 8) return "cold";
  if (/晴/.test(text)) return "clear";
  if (/云|阴|雾|霾/.test(text)) return "cloudy";
  return undefined;
}

function qweatherCodeFailure(code: string | undefined, fallback: string): { ok: false; code: string; message: string } {
  return {
    ok: false,
    code: `QWEATHER_${code ?? "UNKNOWN"}`,
    message: fallback
  };
}

function qweatherUnit(unit: QWeatherUnit): "m" | "i" {
  return unit === "imperial" ? "i" : "m";
}

function readCredential(config: RequiredQWeatherConfig, env: NodeJS.ProcessEnv = process.env): string | undefined {
  const value = env[config.credentialEnv];
  return typeof value === "string" && value.trim() !== "" ? value.trim() : undefined;
}

function normalizeApiHost(value: string | undefined): string | undefined {
  const normalized = readNonEmptyString(value)?.replace(/\/+$/g, "");
  return normalized && /^https?:\/\//.test(normalized) ? normalized : undefined;
}

function readAuthMode(value: unknown): QWeatherAuthMode | undefined {
  return value === "apiKeyHeader" || value === "apiKeyQuery" || value === "jwtBearer" ? value : undefined;
}

function readUnit(value: unknown): QWeatherUnit | undefined {
  return value === "metric" || value === "imperial" ? value : undefined;
}

export function readForecastDays(value: unknown): QWeatherForecastDays | undefined {
  return value === 3 || value === 7 || value === 10 || value === 15 || value === 30 ? value : undefined;
}

function readPositiveNumber(value: unknown): number | undefined {
  return typeof value === "number" && Number.isFinite(value) && value > 0 ? value : undefined;
}

function readNonEmptyString(value: unknown): string | undefined {
  return typeof value === "string" && value.trim() !== "" ? value.trim() : undefined;
}

function readNumber(value: string | undefined): number | undefined {
  if (value === undefined || value.trim() === "") return undefined;
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : undefined;
}

function formatNumber(value: number): string {
  return Number.isInteger(value) ? String(value) : value.toFixed(1);
}

function readErrorName(error: unknown): string | undefined {
  return typeof error === "object" && error !== null && "name" in error && typeof error.name === "string" ? error.name : undefined;
}

function safeErrorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}
