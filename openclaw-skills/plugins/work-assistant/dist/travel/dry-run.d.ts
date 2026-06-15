import type { RouteAdapter, RouteEstimateRequest, RouteEstimateResult, TravelPlannerConfig, UserProfileAdapter, UserProfileReadRequest, UserProfileReadResult, WeatherAdapter, WeatherForecastRequest, WeatherForecastResult } from "./adapters.js";
export declare class DryRunRouteAdapter implements RouteAdapter {
    estimateRoute(request: RouteEstimateRequest): Promise<RouteEstimateResult>;
}
export declare class DryRunWeatherAdapter implements WeatherAdapter {
    getForecast(request: WeatherForecastRequest): Promise<WeatherForecastResult>;
}
export declare class ConfiguredUserProfileAdapter implements UserProfileAdapter {
    private readonly profile;
    constructor(profile?: TravelPlannerConfig);
    readProfile(_request: UserProfileReadRequest): Promise<UserProfileReadResult>;
}
export declare class UnavailableRouteAdapter implements RouteAdapter {
    estimateRoute(_request: RouteEstimateRequest): Promise<RouteEstimateResult>;
}
export declare class UnavailableWeatherAdapter implements WeatherAdapter {
    getForecast(_request: WeatherForecastRequest): Promise<WeatherForecastResult>;
}
export declare function createDryRunUserProfileAdapter(config?: TravelPlannerConfig): UserProfileAdapter;
