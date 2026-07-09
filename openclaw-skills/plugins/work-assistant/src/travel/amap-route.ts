import type {
  AmapRouteConfig,
  RouteAdapter,
  RouteEstimate,
  RouteEstimateRequest,
  RouteEstimateResult,
  RouteMode
} from "./adapters.js";

type AmapAdapterOptions = {
  config: AmapRouteConfig;
  credential?: string;
  fetchImpl?: typeof fetch;
};

type AmapBaseResponse = {
  status?: string;
  info?: string;
  infocode?: string;
};

type AmapGeoResponse = AmapBaseResponse & {
  count?: string;
  geocodes?: Array<{
    formatted_address?: string;
    location?: string | string[];
  }>;
};

type AmapPlaceResponse = AmapBaseResponse & {
  count?: string;
  pois?: Array<{
    name?: string;
    address?: string | string[];
    location?: string | string[];
  }>;
};

type AmapPath = {
  distance?: string;
  duration?: string;
};

type AmapTransit = {
  distance?: string;
  duration?: string;
};

type AmapRouteResponse = AmapBaseResponse & {
  route?: {
    paths?: AmapPath[];
    transits?: AmapTransit[];
  };
};

type ResolvedPoint = {
  location: string;
  label: string;
};

type PointResolveResult =
  | {
      ok: true;
      point: ResolvedPoint;
    }
  | FailureResult;

type FailureResult = {
  ok: false;
  code: string;
  message: string;
};

type RequiredAmapRouteConfig = {
  provider: "amap";
  apiHost: string;
  credentialEnv: string;
  defaultCity?: string;
  originLocation?: string;
  timeoutMs: number;
};

const DEFAULT_API_HOST = "https://restapi.amap.com";
const DEFAULT_CREDENTIAL_ENV = "AMAP_WEB_SERVICE_KEY";
const DEFAULT_TIMEOUT_MS = 5000;

export class AmapRouteAdapter implements RouteAdapter {
  private readonly config: RequiredAmapRouteConfig;
  private readonly credential: string | undefined;
  private readonly fetchImpl: typeof fetch;

  constructor(options: AmapAdapterOptions) {
    this.config = normalizeAmapRouteConfig(options.config);
    this.credential = options.credential ?? readCredential(this.config);
    this.fetchImpl = options.fetchImpl ?? fetch;
  }

  async estimateRoute(request: RouteEstimateRequest): Promise<RouteEstimateResult> {
    const configured = this.checkConfigured();
    if (configured) return configured;

    const origin = await this.resolvePoint(request.origin, "origin");
    if (!origin.ok) return origin;
    const destination = await this.resolvePoint(request.destination, "destination");
    if (!destination.ok) return destination;

    const route = await this.fetchRoute({
      origin: origin.point.location,
      destination: destination.point.location,
      mode: request.mode
    });
    if (!route.ok) return route;

    const estimate: RouteEstimate = {
      origin: request.origin,
      destination: request.destination,
      durationMinutes: route.estimate.durationMinutes,
      mode: request.mode,
      provider: "amap"
    };
    if (route.estimate.distanceMeters !== undefined) estimate.distanceMeters = route.estimate.distanceMeters;
    return {
      ok: true,
      estimate
    };
  }

  private async resolvePoint(
    value: string,
    role: "origin" | "destination"
  ): Promise<PointResolveResult> {
    const direct = parseCoordinate(value);
    if (direct) {
      return {
        ok: true,
        point: {
          location: direct,
          label: value.trim()
        }
      };
    }

    if (role === "origin" && this.config.originLocation) {
      const configuredOrigin = parseCoordinate(this.config.originLocation);
      if (!configuredOrigin) {
        return {
          ok: false,
          code: "AMAP_ORIGIN_LOCATION_INVALID",
          message: "Configured Amap originLocation must be a longitude,latitude coordinate pair."
        };
      }
      return {
        ok: true,
        point: {
          location: configuredOrigin,
          label: value.trim()
        }
      };
    }

    const geocode = await this.geocode(value);
    if (geocode.ok) return geocode;
    if (isProviderFailure(geocode.code)) return geocode;

    const place = await this.searchPlace(value);
    if (place.ok) return place;
    if (isProviderFailure(place.code)) return place;
    return place.code === "AMAP_PLACE_NOT_FOUND" ? geocode : place;
  }

  private async geocode(value: string): Promise<PointResolveResult> {
    const response = await this.requestJson<AmapGeoResponse>("/v3/geocode/geo", {
      address: value,
      city: this.config.defaultCity
    });
    if (!response.ok) return response;
    const status = amapStatusFailure(response.data, "Amap geocode lookup failed.");
    if (status) return status;
    const geocode = response.data.geocodes?.find((item) => readLocation(item.location));
    const location = geocode ? readLocation(geocode.location) : undefined;
    if (!location) {
      return {
        ok: false,
        code: "AMAP_LOCATION_NOT_FOUND",
        message: `Amap geocode did not find a location for "${value}".`
      };
    }
    return {
      ok: true,
      point: {
        location,
        label: geocode?.formatted_address ?? value
      }
    };
  }

  private async searchPlace(value: string): Promise<PointResolveResult> {
    const response = await this.requestJson<AmapPlaceResponse>("/v3/place/text", {
      keywords: value,
      city: this.config.defaultCity,
      citylimit: this.config.defaultCity ? "true" : undefined,
      offset: "1",
      page: "1",
      extensions: "base"
    });
    if (!response.ok) return response;
    const status = amapStatusFailure(response.data, "Amap place lookup failed.");
    if (status) return status;
    const poi = response.data.pois?.find((item) => readLocation(item.location));
    const location = readLocation(poi?.location);
    if (!location) {
      return {
        ok: false,
        code: "AMAP_PLACE_NOT_FOUND",
        message: `Amap place search did not find a location for "${value}".`
      };
    }
    return {
      ok: true,
      point: {
        location,
        label: buildPoiLabel(poi?.name, poi?.address, value)
      }
    };
  }

  private async fetchRoute(request: {
    origin: string;
    destination: string;
    mode: RouteMode;
  }): Promise<RouteEstimateResult> {
    if (request.mode === "transit" && !this.config.defaultCity) {
      return {
        ok: false,
        code: "AMAP_TRANSIT_CITY_MISSING",
        message: "Amap transit route planning requires travel.route.defaultCity."
      };
    }

    const response = await this.requestJson<AmapRouteResponse>(routePath(request.mode), {
      origin: request.origin,
      destination: request.destination,
      city: request.mode === "transit" ? this.config.defaultCity : undefined,
      cityd: request.mode === "transit" ? this.config.defaultCity : undefined,
      extensions: "base"
    });
    if (!response.ok) return response;
    const status = amapStatusFailure(response.data, "Amap route planning failed.");
    if (status) return status;

    const candidate = firstRouteCandidate(response.data, request.mode);
    if (!candidate) {
      return {
        ok: false,
        code: "AMAP_ROUTE_NOT_FOUND",
        message: "Amap route response did not include a usable route."
      };
    }
    const durationSeconds = readPositiveNumber(candidate.duration);
    if (durationSeconds === undefined) {
      return {
        ok: false,
        code: "AMAP_ROUTE_MALFORMED",
        message: "Amap route response did not include route duration."
      };
    }
    const result: RouteEstimateResult = {
      ok: true,
      estimate: {
        origin: request.origin,
        destination: request.destination,
        durationMinutes: Math.max(1, Math.ceil(durationSeconds / 60)),
        mode: request.mode,
        provider: "amap"
      }
    };
    const distanceMeters = readPositiveNumber(candidate.distance);
    if (distanceMeters !== undefined) result.estimate.distanceMeters = Math.round(distanceMeters);
    return result;
  }

  private async requestJson<T>(
    path: string,
    params: Record<string, string | undefined>
  ): Promise<
    | {
        ok: true;
        data: T;
      }
    | FailureResult
  > {
    const url = new URL(`${this.config.apiHost}${path}`);
    for (const [key, value] of Object.entries(params)) {
      if (value !== undefined && value !== "") url.searchParams.set(key, value);
    }
    url.searchParams.set("key", this.credential ?? "");
    url.searchParams.set("output", "json");

    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.config.timeoutMs);
    try {
      const response = await this.fetchImpl(url, {
        headers: {
          Accept: "application/json"
        },
        signal: controller.signal
      });
      const text = await response.text();
      if (!response.ok) {
        return {
          ok: false,
          code: "AMAP_HTTP_ERROR",
          message: `Amap HTTP request failed with status ${response.status}: ${text.slice(0, 120)}`
        };
      }
      try {
        return {
          ok: true,
          data: JSON.parse(text) as T
        };
      } catch (error) {
        return {
          ok: false,
          code: "AMAP_MALFORMED_JSON",
          message: safeErrorMessage(error)
        };
      }
    } catch (error) {
      return {
        ok: false,
        code: readErrorName(error) === "AbortError" ? "AMAP_TIMEOUT" : "AMAP_REQUEST_ERROR",
        message: safeErrorMessage(error)
      };
    } finally {
      clearTimeout(timer);
    }
  }

  private checkConfigured(): RouteEstimateResult | undefined {
    if (!this.config.apiHost) {
      return {
        ok: false,
        code: "AMAP_NOT_CONFIGURED",
        message: "Amap apiHost is not configured."
      };
    }
    if (!this.credential) {
      return {
        ok: false,
        code: "AMAP_CREDENTIAL_MISSING",
        message: `Amap credential environment variable ${this.config.credentialEnv} is not set.`
      };
    }
    return undefined;
  }
}

export function normalizeAmapRouteConfig(config: AmapRouteConfig): RequiredAmapRouteConfig {
  const normalized: RequiredAmapRouteConfig = {
    provider: "amap",
    apiHost: normalizeApiHost(config.apiHost) ?? DEFAULT_API_HOST,
    credentialEnv: readNonEmptyString(config.credentialEnv) ?? DEFAULT_CREDENTIAL_ENV,
    timeoutMs: readPositiveNumber(config.timeoutMs) ?? DEFAULT_TIMEOUT_MS
  };
  const defaultCity = readNonEmptyString(config.defaultCity);
  const originLocation = readNonEmptyString(config.originLocation);
  if (defaultCity) normalized.defaultCity = defaultCity;
  if (originLocation) normalized.originLocation = originLocation;
  return normalized;
}

function routePath(mode: RouteMode): string {
  if (mode === "walking") return "/v3/direction/walking";
  if (mode === "transit") return "/v3/direction/transit/integrated";
  return "/v3/direction/driving";
}

function firstRouteCandidate(response: AmapRouteResponse, mode: RouteMode): AmapPath | AmapTransit | undefined {
  if (mode === "transit") return response.route?.transits?.[0];
  return response.route?.paths?.[0];
}

function amapStatusFailure(response: AmapBaseResponse, fallback: string): FailureResult | undefined {
  if (response.status === "1") return undefined;
  return {
    ok: false,
    code: `AMAP_${response.infocode ?? "UNKNOWN"}`,
    message: response.info ? `${fallback}: ${response.info}` : fallback
  };
}

function isProviderFailure(code: string): boolean {
  return code === "AMAP_HTTP_ERROR" || code === "AMAP_TIMEOUT" || code === "AMAP_REQUEST_ERROR" || /^AMAP_\d/.test(code);
}

function readCredential(config: RequiredAmapRouteConfig, env: NodeJS.ProcessEnv = process.env): string | undefined {
  const value = env[config.credentialEnv];
  return typeof value === "string" && value.trim() !== "" ? value.trim() : undefined;
}

function normalizeApiHost(value: string | undefined): string | undefined {
  const normalized = readNonEmptyString(value)?.replace(/\/+$/g, "");
  return normalized && /^https?:\/\//.test(normalized) ? normalized : undefined;
}

function parseCoordinate(value: string): string | undefined {
  const match = /^\s*(-?\d+(?:\.\d+)?),\s*(-?\d+(?:\.\d+)?)\s*$/.exec(value);
  if (!match) return undefined;
  const lon = Number(match[1]);
  const lat = Number(match[2]);
  if (!Number.isFinite(lon) || !Number.isFinite(lat)) return undefined;
  if (lon < -180 || lon > 180 || lat < -90 || lat > 90) return undefined;
  return `${formatCoordinate(lon)},${formatCoordinate(lat)}`;
}

function readLocation(value: string | string[] | undefined): string | undefined {
  return typeof value === "string" ? parseCoordinate(value) : undefined;
}

function readPositiveNumber(value: string | number | undefined): number | undefined {
  if (value === undefined || value === "") return undefined;
  const parsed = Number(value);
  return Number.isFinite(parsed) && parsed > 0 ? parsed : undefined;
}

function readNonEmptyString(value: unknown): string | undefined {
  return typeof value === "string" && value.trim() !== "" ? value.trim() : undefined;
}

function formatCoordinate(value: number): string {
  return Number.isInteger(value) ? String(value) : String(Number(value.toFixed(6)));
}

function buildPoiLabel(name: string | undefined, address: string | string[] | undefined, fallback: string): string {
  const normalizedAddress = typeof address === "string" ? address.trim() : "";
  if (name && normalizedAddress) return `${name} ${normalizedAddress}`;
  return name ?? fallback;
}

function readErrorName(error: unknown): string | undefined {
  return typeof error === "object" && error !== null && "name" in error && typeof error.name === "string" ? error.name : undefined;
}

function safeErrorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}
