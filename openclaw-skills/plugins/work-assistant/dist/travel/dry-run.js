const DEFAULT_DRY_RUN_PROFILE = {
    originAddress: "上海办公室",
    defaultRouteMode: "driving",
    arrivalBufferMinutes: 15
};
export class DryRunRouteAdapter {
    async estimateRoute(request) {
        const destination = normalizeKey(request.destination);
        if (destination.includes("路线失败") || destination.includes("route-failure")) {
            return {
                ok: false,
                code: "DRY_RUN_ROUTE_FAILURE",
                message: "Dry-run route failure requested by fixture."
            };
        }
        const fixture = routeFixtures().find((item) => destination.includes(item.key));
        if (!fixture) {
            return {
                ok: false,
                code: "ROUTE_NOT_FOUND",
                message: "No dry-run route fixture matched the destination."
            };
        }
        return {
            ok: true,
            estimate: {
                origin: request.origin,
                destination: request.destination,
                durationMinutes: fixture.durationMinutes,
                distanceMeters: fixture.distanceMeters,
                mode: request.mode,
                provider: "dry-run"
            }
        };
    }
}
export class DryRunWeatherAdapter {
    async getForecast(request) {
        const location = normalizeKey(request.location);
        if (location.includes("天气失败") || location.includes("weather-failure")) {
            return {
                ok: false,
                code: "DRY_RUN_WEATHER_FAILURE",
                message: "Dry-run weather failure requested by fixture."
            };
        }
        const fixture = weatherFixtures().find((item) => location.includes(item.key));
        if (!fixture) {
            return {
                ok: false,
                code: "FORECAST_NOT_FOUND",
                message: "No dry-run weather fixture matched the location."
            };
        }
        return {
            ok: true,
            forecast: {
                location: request.location,
                date: request.date,
                summary: fixture.summary,
                condition: fixture.condition,
                lowCelsius: fixture.lowCelsius,
                highCelsius: fixture.highCelsius,
                precipitationChance: fixture.precipitationChance,
                provider: "dry-run"
            }
        };
    }
}
export class ConfiguredUserProfileAdapter {
    profile;
    constructor(profile = {}) {
        this.profile = profile;
    }
    async readProfile(_request) {
        return {
            ok: true,
            profile: {
                ...this.profile
            }
        };
    }
}
export class UnavailableRouteAdapter {
    async estimateRoute(_request) {
        return {
            ok: false,
            code: "ROUTE_PROVIDER_NOT_CONFIGURED",
            message: "No route provider adapter is configured."
        };
    }
}
export class UnavailableWeatherAdapter {
    async getForecast(_request) {
        return {
            ok: false,
            code: "WEATHER_PROVIDER_NOT_CONFIGURED",
            message: "No weather provider adapter is configured."
        };
    }
}
export function createDryRunUserProfileAdapter(config = {}) {
    return new ConfiguredUserProfileAdapter({
        ...DEFAULT_DRY_RUN_PROFILE,
        ...config
    });
}
function normalizeKey(value) {
    return value.trim().toLocaleLowerCase();
}
function routeFixtures() {
    return [
        { key: "客户园区", durationMinutes: 42, distanceMeters: 18500 },
        { key: "北京", durationMinutes: 68, distanceMeters: 38100 },
        { key: "上海办公室", durationMinutes: 20, distanceMeters: 8300 }
    ];
}
function weatherFixtures() {
    return [
        {
            key: "北京",
            summary: "北京明天多云，18 到 27 度，午后有小风。",
            condition: "cloudy",
            lowCelsius: 18,
            highCelsius: 27,
            precipitationChance: 20
        },
        {
            key: "客户园区",
            summary: "客户园区明天有阵雨，22 到 28 度。",
            condition: "rain",
            lowCelsius: 22,
            highCelsius: 28,
            precipitationChance: 70
        },
        {
            key: "上海",
            summary: "上海明天晴到多云，24 到 31 度。",
            condition: "hot",
            lowCelsius: 24,
            highCelsius: 31,
            precipitationChance: 10
        }
    ];
}
//# sourceMappingURL=dry-run.js.map