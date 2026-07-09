const DEFAULT_API_HOST = "https://restapi.amap.com";
const DEFAULT_CREDENTIAL_ENV = "AMAP_WEB_SERVICE_KEY";
const DEFAULT_TIMEOUT_MS = 5000;
export class AmapRouteAdapter {
    config;
    credential;
    fetchImpl;
    constructor(options) {
        this.config = normalizeAmapRouteConfig(options.config);
        this.credential = options.credential ?? readCredential(this.config);
        this.fetchImpl = options.fetchImpl ?? fetch;
    }
    async estimateRoute(request) {
        const configured = this.checkConfigured();
        if (configured)
            return configured;
        const origin = await this.resolvePoint(request.origin, "origin");
        if (!origin.ok)
            return origin;
        const destination = await this.resolvePoint(request.destination, "destination");
        if (!destination.ok)
            return destination;
        const route = await this.fetchRoute({
            origin: origin.point.location,
            destination: destination.point.location,
            mode: request.mode
        });
        if (!route.ok)
            return route;
        const estimate = {
            origin: request.origin,
            destination: request.destination,
            durationMinutes: route.estimate.durationMinutes,
            mode: request.mode,
            provider: "amap"
        };
        if (route.estimate.distanceMeters !== undefined)
            estimate.distanceMeters = route.estimate.distanceMeters;
        return {
            ok: true,
            estimate
        };
    }
    async resolvePoint(value, role) {
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
        if (geocode.ok)
            return geocode;
        if (isProviderFailure(geocode.code))
            return geocode;
        const place = await this.searchPlace(value);
        if (place.ok)
            return place;
        if (isProviderFailure(place.code))
            return place;
        return place.code === "AMAP_PLACE_NOT_FOUND" ? geocode : place;
    }
    async geocode(value) {
        const response = await this.requestJson("/v3/geocode/geo", {
            address: value,
            city: this.config.defaultCity
        });
        if (!response.ok)
            return response;
        const status = amapStatusFailure(response.data, "Amap geocode lookup failed.");
        if (status)
            return status;
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
    async searchPlace(value) {
        const response = await this.requestJson("/v3/place/text", {
            keywords: value,
            city: this.config.defaultCity,
            citylimit: this.config.defaultCity ? "true" : undefined,
            offset: "1",
            page: "1",
            extensions: "base"
        });
        if (!response.ok)
            return response;
        const status = amapStatusFailure(response.data, "Amap place lookup failed.");
        if (status)
            return status;
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
    async fetchRoute(request) {
        if (request.mode === "transit" && !this.config.defaultCity) {
            return {
                ok: false,
                code: "AMAP_TRANSIT_CITY_MISSING",
                message: "Amap transit route planning requires travel.route.defaultCity."
            };
        }
        const response = await this.requestJson(routePath(request.mode), {
            origin: request.origin,
            destination: request.destination,
            city: request.mode === "transit" ? this.config.defaultCity : undefined,
            cityd: request.mode === "transit" ? this.config.defaultCity : undefined,
            extensions: "base"
        });
        if (!response.ok)
            return response;
        const status = amapStatusFailure(response.data, "Amap route planning failed.");
        if (status)
            return status;
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
        const result = {
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
        if (distanceMeters !== undefined)
            result.estimate.distanceMeters = Math.round(distanceMeters);
        return result;
    }
    async requestJson(path, params) {
        const url = new URL(`${this.config.apiHost}${path}`);
        for (const [key, value] of Object.entries(params)) {
            if (value !== undefined && value !== "")
                url.searchParams.set(key, value);
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
                    data: JSON.parse(text)
                };
            }
            catch (error) {
                return {
                    ok: false,
                    code: "AMAP_MALFORMED_JSON",
                    message: safeErrorMessage(error)
                };
            }
        }
        catch (error) {
            return {
                ok: false,
                code: readErrorName(error) === "AbortError" ? "AMAP_TIMEOUT" : "AMAP_REQUEST_ERROR",
                message: safeErrorMessage(error)
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
export function normalizeAmapRouteConfig(config) {
    const normalized = {
        provider: "amap",
        apiHost: normalizeApiHost(config.apiHost) ?? DEFAULT_API_HOST,
        credentialEnv: readNonEmptyString(config.credentialEnv) ?? DEFAULT_CREDENTIAL_ENV,
        timeoutMs: readPositiveNumber(config.timeoutMs) ?? DEFAULT_TIMEOUT_MS
    };
    const defaultCity = readNonEmptyString(config.defaultCity);
    const originLocation = readNonEmptyString(config.originLocation);
    if (defaultCity)
        normalized.defaultCity = defaultCity;
    if (originLocation)
        normalized.originLocation = originLocation;
    return normalized;
}
function routePath(mode) {
    if (mode === "walking")
        return "/v3/direction/walking";
    if (mode === "transit")
        return "/v3/direction/transit/integrated";
    return "/v3/direction/driving";
}
function firstRouteCandidate(response, mode) {
    if (mode === "transit")
        return response.route?.transits?.[0];
    return response.route?.paths?.[0];
}
function amapStatusFailure(response, fallback) {
    if (response.status === "1")
        return undefined;
    return {
        ok: false,
        code: `AMAP_${response.infocode ?? "UNKNOWN"}`,
        message: response.info ? `${fallback}: ${response.info}` : fallback
    };
}
function isProviderFailure(code) {
    return code === "AMAP_HTTP_ERROR" || code === "AMAP_TIMEOUT" || code === "AMAP_REQUEST_ERROR" || /^AMAP_\d/.test(code);
}
function readCredential(config, env = process.env) {
    const value = env[config.credentialEnv];
    return typeof value === "string" && value.trim() !== "" ? value.trim() : undefined;
}
function normalizeApiHost(value) {
    const normalized = readNonEmptyString(value)?.replace(/\/+$/g, "");
    return normalized && /^https?:\/\//.test(normalized) ? normalized : undefined;
}
function parseCoordinate(value) {
    const match = /^\s*(-?\d+(?:\.\d+)?),\s*(-?\d+(?:\.\d+)?)\s*$/.exec(value);
    if (!match)
        return undefined;
    const lon = Number(match[1]);
    const lat = Number(match[2]);
    if (!Number.isFinite(lon) || !Number.isFinite(lat))
        return undefined;
    if (lon < -180 || lon > 180 || lat < -90 || lat > 90)
        return undefined;
    return `${formatCoordinate(lon)},${formatCoordinate(lat)}`;
}
function readLocation(value) {
    return typeof value === "string" ? parseCoordinate(value) : undefined;
}
function readPositiveNumber(value) {
    if (value === undefined || value === "")
        return undefined;
    const parsed = Number(value);
    return Number.isFinite(parsed) && parsed > 0 ? parsed : undefined;
}
function readNonEmptyString(value) {
    return typeof value === "string" && value.trim() !== "" ? value.trim() : undefined;
}
function formatCoordinate(value) {
    return Number.isInteger(value) ? String(value) : String(Number(value.toFixed(6)));
}
function buildPoiLabel(name, address, fallback) {
    const normalizedAddress = typeof address === "string" ? address.trim() : "";
    if (name && normalizedAddress)
        return `${name} ${normalizedAddress}`;
    return name ?? fallback;
}
function readErrorName(error) {
    return typeof error === "object" && error !== null && "name" in error && typeof error.name === "string" ? error.name : undefined;
}
function safeErrorMessage(error) {
    return error instanceof Error ? error.message : String(error);
}
//# sourceMappingURL=amap-route.js.map