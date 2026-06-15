import type {
  RouteAdapter,
  RouteEstimateRequest,
  RouteEstimateResult,
  TravelPlannerConfig,
  UserProfileAdapter,
  UserProfileReadRequest,
  UserProfileReadResult,
  WeatherAdapter,
  WeatherForecastRequest,
  WeatherForecastResult
} from "./adapters.js";

const DEFAULT_DRY_RUN_PROFILE: TravelPlannerConfig = {
  originAddress: "上海办公室",
  defaultRouteMode: "driving",
  arrivalBufferMinutes: 15
};

export class DryRunRouteAdapter implements RouteAdapter {
  async estimateRoute(request: RouteEstimateRequest): Promise<RouteEstimateResult> {
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

export class DryRunWeatherAdapter implements WeatherAdapter {
  async getForecast(request: WeatherForecastRequest): Promise<WeatherForecastResult> {
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

export class ConfiguredUserProfileAdapter implements UserProfileAdapter {
  constructor(private readonly profile: TravelPlannerConfig = {}) {}

  async readProfile(_request: UserProfileReadRequest): Promise<UserProfileReadResult> {
    return {
      ok: true,
      profile: {
        ...this.profile
      }
    };
  }
}

export class UnavailableRouteAdapter implements RouteAdapter {
  async estimateRoute(_request: RouteEstimateRequest): Promise<RouteEstimateResult> {
    return {
      ok: false,
      code: "ROUTE_PROVIDER_NOT_CONFIGURED",
      message: "No route provider adapter is configured."
    };
  }
}

export class UnavailableWeatherAdapter implements WeatherAdapter {
  async getForecast(_request: WeatherForecastRequest): Promise<WeatherForecastResult> {
    return {
      ok: false,
      code: "WEATHER_PROVIDER_NOT_CONFIGURED",
      message: "No weather provider adapter is configured."
    };
  }
}

export function createDryRunUserProfileAdapter(config: TravelPlannerConfig = {}): UserProfileAdapter {
  return new ConfiguredUserProfileAdapter({
    ...DEFAULT_DRY_RUN_PROFILE,
    ...config
  });
}

function normalizeKey(value: string): string {
  return value.trim().toLocaleLowerCase();
}

function routeFixtures(): Array<{ key: string; durationMinutes: number; distanceMeters: number }> {
  return [
    { key: "客户园区", durationMinutes: 42, distanceMeters: 18500 },
    { key: "北京", durationMinutes: 68, distanceMeters: 38100 },
    { key: "上海办公室", durationMinutes: 20, distanceMeters: 8300 }
  ];
}

function weatherFixtures(): Array<{
  key: string;
  summary: string;
  condition: "clear" | "cloudy" | "rain" | "snow" | "wind" | "hot" | "cold";
  lowCelsius: number;
  highCelsius: number;
  precipitationChance: number;
}> {
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
