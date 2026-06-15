export type RouteMode = "driving" | "transit" | "walking";

export type RouteEstimateRequest = {
  origin: string;
  destination: string;
  departAt: string;
  mode: RouteMode;
};

export type RouteEstimate = {
  origin: string;
  destination: string;
  durationMinutes: number;
  mode: RouteMode;
  distanceMeters?: number;
  provider?: string;
};

export type RouteEstimateResult =
  | {
      ok: true;
      estimate: RouteEstimate;
    }
  | {
      ok: false;
      code: string;
      message: string;
    };

export interface RouteAdapter {
  estimateRoute(request: RouteEstimateRequest): Promise<RouteEstimateResult>;
}

export type WeatherForecastRequest = {
  location: string;
  date: string;
};

export type WeatherForecast = {
  location: string;
  date: string;
  summary: string;
  condition?: "clear" | "cloudy" | "rain" | "snow" | "wind" | "hot" | "cold";
  lowCelsius?: number;
  highCelsius?: number;
  precipitationChance?: number;
  provider?: string;
};

export type WeatherForecastResult =
  | {
      ok: true;
      forecast: WeatherForecast;
    }
  | {
      ok: false;
      code: string;
      message: string;
    };

export interface WeatherAdapter {
  getForecast(request: WeatherForecastRequest): Promise<WeatherForecastResult>;
}

export type QWeatherAuthMode = "apiKeyHeader" | "apiKeyQuery" | "jwtBearer";
export type QWeatherUnit = "metric" | "imperial";
export type QWeatherForecastDays = 3 | 7 | 10 | 15 | 30;

export type QWeatherWeatherConfig = {
  provider: "qweather";
  apiHost?: string;
  credentialEnv?: string;
  authMode?: QWeatherAuthMode;
  defaultLanguage?: string;
  defaultUnit?: QWeatherUnit;
  defaultRange?: string;
  forecastDays?: QWeatherForecastDays;
  timeoutMs?: number;
};

export type UserProfile = {
  originAddress?: string;
  homeCity?: string;
  defaultRouteMode?: RouteMode;
  arrivalBufferMinutes?: number;
};

export type UserProfileReadRequest = {
  userId: string;
};

export type UserProfileReadResult =
  | {
      ok: true;
      profile: UserProfile;
    }
  | {
      ok: false;
      code: string;
      message: string;
    };

export interface UserProfileAdapter {
  readProfile(request: UserProfileReadRequest): Promise<UserProfileReadResult>;
}

export type DestinationSummary = {
  destination: string;
  source: "location" | "description" | "title";
};

export type TravelContextPatch = {
  current_focus?: {
    type: "travel_event";
    event_id: string;
    calendar_id?: string;
    title: string;
    start_time: string;
    end_time: string;
    destination?: string;
    recommended_departure_time?: string;
  };
  travel_summary?: {
    source_event_id: string;
    title: string;
    destination?: string;
    trip_date?: string;
    weather_status?: string;
  };
};

export type TravelPlannerConfig = {
  originAddress?: string;
  defaultRouteMode?: RouteMode;
  arrivalBufferMinutes?: number;
  weather?: QWeatherWeatherConfig;
};
