import type { AmapRouteConfig, RouteAdapter, RouteEstimateRequest, RouteEstimateResult } from "./adapters.js";
type AmapAdapterOptions = {
    config: AmapRouteConfig;
    credential?: string;
    fetchImpl?: typeof fetch;
};
type RequiredAmapRouteConfig = {
    provider: "amap";
    apiHost: string;
    credentialEnv: string;
    defaultCity?: string;
    originLocation?: string;
    timeoutMs: number;
};
export declare class AmapRouteAdapter implements RouteAdapter {
    private readonly config;
    private readonly credential;
    private readonly fetchImpl;
    constructor(options: AmapAdapterOptions);
    estimateRoute(request: RouteEstimateRequest): Promise<RouteEstimateResult>;
    private resolvePoint;
    private geocode;
    private searchPlace;
    private fetchRoute;
    private requestJson;
    private checkConfigured;
}
export declare function normalizeAmapRouteConfig(config: AmapRouteConfig): RequiredAmapRouteConfig;
export {};
