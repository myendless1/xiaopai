import type { InputEvent, StructuredResponse } from "../contracts.js";
import type { RouteAdapter, TravelPlannerConfig, UserProfileAdapter, WeatherAdapter } from "./adapters.js";
export type TravelPlannerAssistantOptions = TravelPlannerConfig & {
    routeAdapter: RouteAdapter;
    weatherAdapter: WeatherAdapter;
    userProfileAdapter: UserProfileAdapter;
};
export declare class TravelPlannerAssistant {
    private readonly routeAdapter;
    private readonly weatherAdapter;
    private readonly userProfileAdapter;
    private readonly config;
    constructor(options: TravelPlannerAssistantOptions);
    handleOutdoorEvent(event: InputEvent): Promise<StructuredResponse>;
    handleBusinessTripTomorrow(event: InputEvent): Promise<StructuredResponse>;
    private readPreferences;
    private estimateRoute;
    private getForecast;
}
