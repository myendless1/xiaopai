export class QWeatherClient {
    config;
    credential;
    fetchImpl;
    constructor(options) {
        this.config = options.config;
        this.credential = options.credential;
        this.fetchImpl = options.fetchImpl ?? fetch;
    }
    async getCurrent(request) {
        const checked = this.checkConfigured();
        if (checked)
            return checked;
        const location = await this.lookupLocation(request);
        if (!location.ok)
            return location;
        const language = request.language ?? this.config.defaultLanguage;
        const unit = request.unit ?? this.config.defaultUnit;
        const response = await this.requestJson({
            path: "/v7/weather/now",
            params: {
                location: location.location.id,
                lang: language,
                unit: qweatherUnit(unit)
            }
        });
        if (!response.ok)
            return qweatherFailure(response.error);
        if (response.data.code !== "200")
            return qweatherFailure(qweatherCodeError(response.data.code, "QWeather current weather lookup failed."));
        if (!response.data.now) {
            return qweatherFailure({ code: "QWEATHER_MALFORMED_RESPONSE", message: "QWeather response did not include now weather data." });
        }
        const current = normalizeCurrent(location.location, response.data.now);
        return {
            ok: true,
            current
        };
    }
    async getForecast(request) {
        const checked = this.checkConfigured();
        if (checked)
            return checked;
        const location = await this.lookupLocation(request);
        if (!location.ok)
            return location;
        const days = request.days ?? this.config.forecastDays;
        const language = request.language ?? this.config.defaultLanguage;
        const unit = request.unit ?? this.config.defaultUnit;
        const response = await this.requestJson({
            path: `/v7/weather/${days}d`,
            params: {
                location: location.location.id,
                lang: language,
                unit: qweatherUnit(unit)
            }
        });
        if (!response.ok)
            return qweatherFailure(response.error);
        if (response.data.code !== "200")
            return qweatherFailure(qweatherCodeError(response.data.code, "QWeather daily forecast lookup failed."));
        const daily = Array.isArray(response.data.daily) ? response.data.daily : [];
        const forecast = daily
            .map((item) => normalizeDailyForecast(location.location, item))
            .filter((item) => Boolean(item));
        if (forecast.length === 0) {
            return qweatherFailure({ code: "QWEATHER_MALFORMED_RESPONSE", message: "QWeather response did not include daily forecast data." });
        }
        const selected = request.date ? forecast.find((item) => item.date === request.date) : forecast[0];
        const result = {
            ok: true,
            forecast,
            summary: selected?.summary ?? forecast[0]?.summary ?? ""
        };
        if (selected)
            result.selected = selected;
        return result;
    }
    async lookupLocation(request) {
        const checked = this.checkConfigured();
        if (checked)
            return checked;
        const direct = resolveDirectLocation(request.location);
        if (direct) {
            return {
                ok: true,
                location: direct
            };
        }
        const known = resolveKnownLocation(request.location);
        const language = request.language ?? this.config.defaultLanguage;
        const response = await this.requestJson({
            path: "/geo/v2/city/lookup",
            params: {
                location: request.location,
                adm: request.adm,
                range: request.range ?? this.config.defaultRange,
                number: "1",
                lang: language
            }
        });
        if (!response.ok) {
            if (known)
                return { ok: true, location: known };
            return qweatherFailure(response.error);
        }
        if (response.data.code !== "200") {
            if (known)
                return { ok: true, location: known };
            return qweatherFailure(qweatherCodeError(response.data.code, "QWeather location lookup failed."));
        }
        const first = response.data.location?.[0];
        const location = normalizeLocation(first);
        if (!location && known)
            return { ok: true, location: known };
        if (!location) {
            return qweatherFailure({
                code: "QWEATHER_LOCATION_NOT_FOUND",
                message: `No QWeather location matched "${request.location}".`
            });
        }
        return {
            ok: true,
            location
        };
    }
    async requestJson(options) {
        const url = new URL(`${this.config.apiHost}${options.path}`);
        for (const [key, value] of Object.entries(options.params)) {
            if (value !== undefined && value !== "")
                url.searchParams.set(key, value);
        }
        const headers = new Headers({
            Accept: "application/json"
        });
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
                    error: {
                        code: "QWEATHER_HTTP_ERROR",
                        message: `QWeather HTTP request failed with status ${response.status}.`,
                        details: { status: response.status, body: text.slice(0, 300) }
                    }
                };
            }
            return {
                ok: true,
                data: JSON.parse(text)
            };
        }
        catch (error) {
            return {
                ok: false,
                error: {
                    code: error instanceof DOMException && error.name === "AbortError" ? "QWEATHER_TIMEOUT" : "QWEATHER_REQUEST_ERROR",
                    message: safeErrorMessage(error)
                }
            };
        }
        finally {
            clearTimeout(timer);
        }
    }
    checkConfigured() {
        if (!this.config.apiHost) {
            return {
                ok: false,
                code: "QWEATHER_NOT_CONFIGURED",
                message: "QWeather apiHost is not configured.",
                provider: "qweather"
            };
        }
        if (!this.credential) {
            return {
                ok: false,
                code: "QWEATHER_CREDENTIAL_MISSING",
                message: `QWeather credential environment variable ${this.config.credentialEnv} is not set.`,
                provider: "qweather"
            };
        }
        return undefined;
    }
    applyAuth(url, headers) {
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
}
function qweatherFailure(error) {
    const failure = {
        ok: false,
        code: error.code,
        message: error.message,
        provider: "qweather"
    };
    if (error.details)
        failure.details = error.details;
    return failure;
}
function qweatherCodeError(code, fallback) {
    const error = {
        code: `QWEATHER_${code ?? "UNKNOWN"}`,
        message: fallback
    };
    if (code)
        error.details = { qweather_code: code };
    return error;
}
function qweatherUnit(unit) {
    return unit === "imperial" ? "i" : "m";
}
function normalizeLocation(value) {
    if (!value?.id || !value.name)
        return undefined;
    const location = {
        id: value.id,
        name: value.name
    };
    if (value.country)
        location.country = value.country;
    if (value.adm1)
        location.adm1 = value.adm1;
    if (value.adm2)
        location.adm2 = value.adm2;
    if (value.tz)
        location.timezone = value.tz;
    if (value.fxLink)
        location.link = value.fxLink;
    const latitude = readNumber(value.lat);
    const longitude = readNumber(value.lon);
    if (latitude !== undefined)
        location.latitude = latitude;
    if (longitude !== undefined)
        location.longitude = longitude;
    return location;
}
function resolveDirectLocation(value) {
    const trimmed = value.trim();
    if (!/^\d{6,12}$/.test(trimmed))
        return undefined;
    return {
        id: trimmed,
        name: trimmed
    };
}
function resolveKnownLocation(value) {
    const key = normalizeLocationKey(value);
    const match = KNOWN_CHINA_LOCATION_IDS.find((item) => item.aliases.some((alias) => normalizeLocationKey(alias) === key));
    return match
        ? {
            id: match.id,
            name: match.name,
            country: "中国"
        }
        : undefined;
}
const KNOWN_CHINA_LOCATION_IDS = [
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
function normalizeLocationKey(value) {
    return value.trim().toLocaleLowerCase().replace(/\s+/g, "").replace(/市$/u, "");
}
function normalizeCurrent(location, now) {
    const temp = readNumber(now.temp);
    const feelsLike = readNumber(now.feelsLike);
    const humidity = readNumber(now.humidity);
    const windSpeed = readNumber(now.windSpeed);
    const text = now.text ?? "天气状况未知";
    const current = {
        provider: "qweather",
        location,
        observedAt: now.obsTime ?? new Date().toISOString(),
        summary: buildCurrentSummary(location.name, text, temp, feelsLike, humidity)
    };
    const condition = inferCondition(text, temp, temp);
    if (condition)
        current.condition = condition;
    if (temp !== undefined)
        current.tempCelsius = temp;
    if (feelsLike !== undefined)
        current.feelsLikeCelsius = feelsLike;
    if (humidity !== undefined)
        current.humidityPercent = humidity;
    if (now.windDir)
        current.windDirection = now.windDir;
    if (now.windScale)
        current.windScale = now.windScale;
    if (windSpeed !== undefined)
        current.windSpeedKph = windSpeed;
    return current;
}
function normalizeDailyForecast(location, item) {
    if (!item.fxDate)
        return undefined;
    const low = readNumber(item.tempMin);
    const high = readNumber(item.tempMax);
    const pop = readNumber(item.precipProbability);
    const humidity = readNumber(item.humidity);
    const text = normalizeDayNightText(item.textDay, item.textNight);
    const forecast = {
        provider: "qweather",
        location,
        date: item.fxDate,
        summary: buildForecastSummary(location.name, item.fxDate, text, low, high, pop)
    };
    const condition = inferCondition(text, high, low);
    if (condition)
        forecast.condition = condition;
    if (low !== undefined)
        forecast.lowCelsius = low;
    if (high !== undefined)
        forecast.highCelsius = high;
    if (pop !== undefined)
        forecast.precipitationChance = pop;
    if (humidity !== undefined)
        forecast.humidityPercent = humidity;
    if (item.windDirDay)
        forecast.windDirection = item.windDirDay;
    if (item.windScaleDay)
        forecast.windScale = item.windScaleDay;
    return forecast;
}
function normalizeDayNightText(day, night) {
    if (day && night && day !== night)
        return `${day}转${night}`;
    return day ?? night ?? "天气状况未知";
}
function buildCurrentSummary(locationName, text, temp, feelsLike, humidity) {
    const parts = [`${locationName}当前${text}`];
    if (temp !== undefined)
        parts.push(`${formatNumber(temp)}度`);
    if (feelsLike !== undefined)
        parts.push(`体感${formatNumber(feelsLike)}度`);
    if (humidity !== undefined)
        parts.push(`湿度${formatNumber(humidity)}%`);
    return `${parts.join("，")}。`;
}
function buildForecastSummary(locationName, date, text, low, high, pop) {
    const parts = [`${locationName}${date}天气${text}`];
    if (low !== undefined && high !== undefined)
        parts.push(`${formatNumber(low)}到${formatNumber(high)}度`);
    if (pop !== undefined)
        parts.push(`降水概率${formatNumber(pop)}%`);
    return `${parts.join("，")}。`;
}
function inferCondition(text, high, low) {
    if (/雪|霰|冰雹/.test(text))
        return "snow";
    if (/雨|雷|阵雨|暴雨/.test(text))
        return "rain";
    if (/风|沙|尘|扬沙|浮尘/.test(text))
        return "wind";
    if ((high ?? low ?? 0) >= 32)
        return "hot";
    if ((low ?? high ?? 99) <= 8)
        return "cold";
    if (/晴/.test(text))
        return "clear";
    if (/云|阴|雾|霾/.test(text))
        return "cloudy";
    return undefined;
}
function readNumber(value) {
    if (value === undefined || value.trim() === "")
        return undefined;
    const parsed = Number(value);
    return Number.isFinite(parsed) ? parsed : undefined;
}
function formatNumber(value) {
    return Number.isInteger(value) ? String(value) : value.toFixed(1);
}
function safeErrorMessage(error) {
    return error instanceof Error ? error.message : String(error);
}
export function isForecastDays(value) {
    return value === 3 || value === 7 || value === 10 || value === 15 || value === 30;
}
export function isAuthMode(value) {
    return value === "apiKeyHeader" || value === "apiKeyQuery" || value === "jwtBearer";
}
//# sourceMappingURL=qweather.js.map