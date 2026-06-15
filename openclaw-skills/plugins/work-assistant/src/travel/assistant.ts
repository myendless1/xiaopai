import type {
  InputEvent,
  SchedulerCalendarEventPayload,
  SchedulerTriggerPayload,
  StructuredResponse,
  ToolAction
} from "../contracts.js";
import type {
  DestinationSummary,
  RouteAdapter,
  RouteEstimate,
  RouteMode,
  TravelPlannerConfig,
  UserProfile,
  UserProfileAdapter,
  WeatherAdapter,
  WeatherForecast
} from "./adapters.js";

export type TravelPlannerAssistantOptions = TravelPlannerConfig & {
  routeAdapter: RouteAdapter;
  weatherAdapter: WeatherAdapter;
  userProfileAdapter: UserProfileAdapter;
};

type NormalizedTravelEvent = {
  id: string;
  title: string;
  start: string;
  end: string;
  calendarId?: string;
  location?: string;
  description?: string;
  triggerKey?: string;
  sourceEventId?: string;
};

type TravelPreferences = {
  originAddress?: string;
  routeMode: RouteMode;
  arrivalBufferMinutes: number;
  profileAction: ToolAction;
};

const DEFAULT_ROUTE_MODE: RouteMode = "driving";
const DEFAULT_ARRIVAL_BUFFER_MINUTES = 15;

export class TravelPlannerAssistant {
  private readonly routeAdapter: RouteAdapter;
  private readonly weatherAdapter: WeatherAdapter;
  private readonly userProfileAdapter: UserProfileAdapter;
  private readonly config: TravelPlannerConfig;

  constructor(options: TravelPlannerAssistantOptions) {
    this.routeAdapter = options.routeAdapter;
    this.weatherAdapter = options.weatherAdapter;
    this.userProfileAdapter = options.userProfileAdapter;
    this.config = {
      ...(options.originAddress ? { originAddress: options.originAddress } : {}),
      ...(options.defaultRouteMode ? { defaultRouteMode: options.defaultRouteMode } : {}),
      ...(options.arrivalBufferMinutes ? { arrivalBufferMinutes: options.arrivalBufferMinutes } : {})
    };
  }

  async handleOutdoorEvent(event: InputEvent): Promise<StructuredResponse> {
    const calendarEvent = normalizeCalendarEvent(event.payload.calendar_event, event.payload.trigger);
    if (!calendarEvent) return malformedResponse("outdoor");

    const destination = resolveDestination(calendarEvent, "outdoor");
    if (!destination) {
      return outdoorMissingDestinationResponse(calendarEvent);
    }

    const actions: ToolAction[] = [];
    const preferences = await this.readPreferences(event.user_id);
    actions.push(preferences.profileAction);

    let route: RouteEstimate | undefined;
    let recommendedDepartureTime: string | undefined;
    if (!preferences.originAddress) {
      actions.push(skippedRouteAction(calendarEvent, destination, "MISSING_ORIGIN", "Origin address is not configured or available in profile."));
    } else {
      const routeAction = await this.estimateRoute(calendarEvent, destination, preferences);
      actions.push(routeAction.action);
      if (routeAction.route) {
        route = routeAction.route;
        recommendedDepartureTime = calculateDepartureTime(
          calendarEvent.start,
          route.durationMinutes,
          preferences.arrivalBufferMinutes
        );
      }
    }

    actions.push({
      type: "travel.plan.generate",
      status: "success",
      details: {
        travel_type: "outdoor_event",
        event_id: calendarEvent.id,
        calendar_id: calendarEvent.calendarId ?? "primary",
        title: calendarEvent.title,
        destination: destination.destination,
        destination_source: destination.source,
        start: calendarEvent.start,
        trigger_key: calendarEvent.triggerKey,
        route_status: route ? "available" : "unavailable",
        recommended_departure_time: recommendedDepartureTime
      }
    });

    return {
      speech: buildOutdoorSpeech(calendarEvent, destination.destination, event.context.timezone, route, recommendedDepartureTime),
      presentation: {
        emotion: route ? "focused" : "thinking",
        motion: "look_at_user",
        light: route ? "soft_blink" : "amber"
      },
      actions,
      follow_up: {
        expected: false
      },
      context_patch: {
        current_focus: buildTravelFocus(calendarEvent, destination.destination, recommendedDepartureTime),
        travel_summary: {
          source_event_id: calendarEvent.id,
          title: calendarEvent.title,
          destination: destination.destination,
          trip_date: localDate(calendarEvent.start)
        }
      }
    };
  }

  async handleBusinessTripTomorrow(event: InputEvent): Promise<StructuredResponse> {
    const calendarEvent = normalizeCalendarEvent(event.payload.calendar_event, event.payload.trigger);
    if (!calendarEvent) return malformedResponse("business_trip");

    const destination = resolveDestination(calendarEvent, "business_trip");
    if (!destination) {
      return businessTripMissingDestinationResponse(calendarEvent, event.context.timezone);
    }

    const actions: ToolAction[] = [];
    const forecastAction = await this.getForecast(calendarEvent, destination);
    actions.push(forecastAction.action);
    actions.push({
      type: "travel.plan.generate",
      status: "success",
      details: {
        travel_type: "business_trip_tomorrow",
        event_id: calendarEvent.id,
        calendar_id: calendarEvent.calendarId ?? "primary",
        title: calendarEvent.title,
        destination: destination.destination,
        destination_source: destination.source,
        start: calendarEvent.start,
        trigger_key: calendarEvent.triggerKey,
        weather_status: forecastAction.forecast ? "available" : "unavailable"
      }
    });

    return {
      speech: buildBusinessTripSpeech(
        calendarEvent,
        destination.destination,
        event.context.timezone,
        forecastAction.forecast
      ),
      presentation: {
        emotion: forecastAction.forecast ? "focused" : "thinking",
        motion: "look_at_user",
        light: forecastAction.forecast ? "soft_blink" : "amber"
      },
      actions,
      follow_up: {
        expected: false
      },
      context_patch: {
        travel_summary: {
          source_event_id: calendarEvent.id,
          title: calendarEvent.title,
          destination: destination.destination,
          trip_date: localDate(calendarEvent.start),
          weather_status: forecastAction.forecast ? forecastAction.forecast.summary : "unavailable"
        }
      }
    };
  }

  private async readPreferences(userId: string): Promise<TravelPreferences> {
    try {
      const result = await this.userProfileAdapter.readProfile({ userId });
      if (!result.ok) {
        return {
          routeMode: this.config.defaultRouteMode ?? DEFAULT_ROUTE_MODE,
          arrivalBufferMinutes: this.config.arrivalBufferMinutes ?? DEFAULT_ARRIVAL_BUFFER_MINUTES,
          profileAction: {
            type: "user.profile.read",
            status: "failed",
            error: {
              code: result.code,
              message: result.message
            }
          }
        };
      }
      const merged = mergeProfile(this.config, result.profile);
      const response: TravelPreferences = {
        routeMode: merged.defaultRouteMode ?? DEFAULT_ROUTE_MODE,
        arrivalBufferMinutes: merged.arrivalBufferMinutes ?? DEFAULT_ARRIVAL_BUFFER_MINUTES,
        profileAction: {
          type: "user.profile.read",
          status: "success",
          details: {
            has_origin_address: Boolean(merged.originAddress),
            route_mode: merged.defaultRouteMode ?? DEFAULT_ROUTE_MODE,
            arrival_buffer_minutes: merged.arrivalBufferMinutes ?? DEFAULT_ARRIVAL_BUFFER_MINUTES
          }
        }
      };
      if (merged.originAddress) response.originAddress = merged.originAddress;
      return response;
    } catch (error) {
      return {
        routeMode: this.config.defaultRouteMode ?? DEFAULT_ROUTE_MODE,
        arrivalBufferMinutes: this.config.arrivalBufferMinutes ?? DEFAULT_ARRIVAL_BUFFER_MINUTES,
        profileAction: {
          type: "user.profile.read",
          status: "failed",
          error: {
            code: "PROFILE_ADAPTER_ERROR",
            message: safeErrorMessage(error)
          }
        }
      };
    }
  }

  private async estimateRoute(
    calendarEvent: NormalizedTravelEvent,
    destination: DestinationSummary,
    preferences: TravelPreferences
  ): Promise<{ action: ToolAction; route?: RouteEstimate }> {
    if (!preferences.originAddress) {
      return {
        action: skippedRouteAction(calendarEvent, destination, "MISSING_ORIGIN", "Origin address is not configured or available in profile.")
      };
    }
    try {
      const result = await this.routeAdapter.estimateRoute({
        origin: preferences.originAddress,
        destination: destination.destination,
        departAt: calendarEvent.start,
        mode: preferences.routeMode
      });
      if (!result.ok) {
        return {
          action: {
            type: "route.estimate",
            status: "failed",
            error: {
              code: result.code,
              message: result.message
            },
            details: routeActionDetails(calendarEvent, destination, preferences)
          }
        };
      }
      return {
        route: result.estimate,
        action: {
          type: "route.estimate",
          status: "success",
          details: {
            ...routeActionDetails(calendarEvent, destination, preferences),
            duration_minutes: result.estimate.durationMinutes,
            distance_meters: result.estimate.distanceMeters,
            provider: result.estimate.provider
          }
        }
      };
    } catch (error) {
      return {
        action: {
          type: "route.estimate",
          status: "failed",
          error: {
            code: "ROUTE_ADAPTER_ERROR",
            message: safeErrorMessage(error)
          },
          details: routeActionDetails(calendarEvent, destination, preferences)
        }
      };
    }
  }

  private async getForecast(
    calendarEvent: NormalizedTravelEvent,
    destination: DestinationSummary
  ): Promise<{ action: ToolAction; forecast?: WeatherForecast }> {
    try {
      const result = await this.weatherAdapter.getForecast({
        location: destination.destination,
        date: localDate(calendarEvent.start)
      });
      if (!result.ok) {
        return {
          action: {
            type: "weather.forecast",
            status: "failed",
            error: {
              code: result.code,
              message: result.message
            },
            details: weatherActionDetails(calendarEvent, destination)
          }
        };
      }
      return {
        forecast: result.forecast,
        action: {
          type: "weather.forecast",
          status: "success",
          details: {
            ...weatherActionDetails(calendarEvent, destination),
            summary: result.forecast.summary,
            condition: result.forecast.condition,
            low_celsius: result.forecast.lowCelsius,
            high_celsius: result.forecast.highCelsius,
            precipitation_chance: result.forecast.precipitationChance,
            provider: result.forecast.provider
          }
        }
      };
    } catch (error) {
      return {
        action: {
          type: "weather.forecast",
          status: "failed",
          error: {
            code: "WEATHER_ADAPTER_ERROR",
            message: safeErrorMessage(error)
          },
          details: weatherActionDetails(calendarEvent, destination)
        }
      };
    }
  }
}

function normalizeCalendarEvent(value: unknown, triggerValue: unknown): NormalizedTravelEvent | undefined {
  if (!isRecord(value)) return undefined;
  const id = readString(value.id);
  const title = readString(value.title);
  const start = readString(value.start);
  const end = readString(value.end);
  if (!id || !title || !start || !end || !isValidDate(start) || !isValidDate(end)) return undefined;
  const event: NormalizedTravelEvent = {
    id,
    title,
    start,
    end
  };
  const calendarId = readString(value.calendar_id) ?? readString(value.calendarId);
  const location = readString(value.location);
  const description = readString(value.description);
  const trigger = normalizeTrigger(triggerValue);
  if (calendarId) event.calendarId = calendarId;
  if (location) event.location = location;
  if (description) event.description = description;
  if (trigger?.triggerKey) event.triggerKey = trigger.triggerKey;
  if (trigger?.sourceEventId) event.sourceEventId = trigger.sourceEventId;
  return event;
}

function normalizeTrigger(value: unknown): { triggerKey?: string; sourceEventId?: string } | undefined {
  if (!isRecord(value)) return undefined;
  const result: { triggerKey?: string; sourceEventId?: string } = {};
  const triggerKey = readString(value.trigger_key);
  const sourceEventId = readString(value.source_event_id);
  if (triggerKey) result.triggerKey = triggerKey;
  if (sourceEventId) result.sourceEventId = sourceEventId;
  return result;
}

function resolveDestination(event: NormalizedTravelEvent, kind: "outdoor" | "business_trip"): DestinationSummary | undefined {
  if (event.location) return { destination: event.location, source: "location" };
  const fromDescription = extractDestinationFromText(event.description);
  if (fromDescription) return { destination: fromDescription, source: "description" };
  const fromTitle = kind === "outdoor" ? extractOutdoorDestinationFromTitle(event.title) : extractTripDestinationFromTitle(event.title);
  return fromTitle ? { destination: fromTitle, source: "title" } : undefined;
}

function extractDestinationFromText(value: string | undefined): string | undefined {
  if (!value) return undefined;
  const match = value.match(/(?:前往|去|到|赴|地点[:：]?)([^，。；;,\n]{2,30})/);
  return cleanDestination(match?.[1]);
}

function extractOutdoorDestinationFromTitle(title: string): string | undefined {
  const explicit = title.match(/(?:前往|去|到|赴)([^，。；;,\n]{2,30})/);
  const explicitDestination = cleanDestination(explicit?.[1]);
  if (explicitDestination) return explicitDestination;
  const known = ["客户园区", "园区", "客户现场", "客户办公室"];
  return known.find((item) => title.includes(item));
}

function extractTripDestinationFromTitle(title: string): string | undefined {
  const explicit = title.match(/(?:前往|去|到|赴)([^，。；;,\n]{2,30})/);
  const explicitDestination = cleanDestination(explicit?.[1]);
  if (explicitDestination) return explicitDestination;
  const prefix = title.match(/^([^，。；;,\n]{2,12})(?:出差|差旅|商务旅行|客户拜访)/);
  const prefixDestination = cleanDestination(prefix?.[1]);
  if (prefixDestination) return prefixDestination;
  const suffix = title.match(/(?:出差|差旅|商务旅行)(?:到|去|赴)?([^，。；;,\n]{2,12})/);
  return cleanDestination(suffix?.[1]);
}

function cleanDestination(value: string | undefined): string | undefined {
  if (!value) return undefined;
  const cleaned = value
    .replace(/(?:拜访|出差|差旅|客户会议|会议|沟通|现场|准备|材料|正装|文件|合同|方案)+$/g, "")
    .trim();
  return cleaned.length >= 2 ? cleaned.slice(0, 30) : undefined;
}

function malformedResponse(kind: "outdoor" | "business_trip"): StructuredResponse {
  const text = kind === "outdoor" ? "外出提醒数据不完整，我暂时不能生成路线提醒。" : "出差提醒数据不完整，我暂时不能生成出行提醒。";
  return {
    speech: text,
    presentation: {
      emotion: "concerned",
      motion: "none",
      light: "amber"
    },
    actions: [
      {
        type: "travel.plan.generate",
        status: "failed",
        error: {
          code: "MALFORMED_TRAVEL_EVENT",
          message: "payload.calendar_event must include id, title, start, and end."
        },
        details: {
          travel_type: kind
        }
      }
    ],
    follow_up: {
      expected: false
    },
    context_patch: {}
  };
}

function outdoorMissingDestinationResponse(event: NormalizedTravelEvent): StructuredResponse {
  return {
    speech: `提醒你，${formatLocalTime(event.start, "Asia/Shanghai")}有${event.title}，但日程里没有明确地点，请出发前确认目的地。`,
    presentation: {
      emotion: "thinking",
      motion: "look_at_user",
      light: "amber"
    },
    actions: [
      {
        type: "route.estimate",
        status: "skipped",
        details: {
          reason: "missing_destination",
          event_id: event.id,
          title: event.title,
          trigger_key: event.triggerKey
        }
      },
      {
        type: "travel.plan.generate",
        status: "failed",
        error: {
          code: "MISSING_DESTINATION",
          message: "No usable destination was resolved from calendar location, description, or title."
        },
        details: {
          travel_type: "outdoor_event",
          event_id: event.id,
          title: event.title,
          start: event.start,
          trigger_key: event.triggerKey
        }
      }
    ],
    follow_up: {
      expected: false
    },
    context_patch: {
      travel_summary: {
        source_event_id: event.id,
        title: event.title,
        trip_date: localDate(event.start)
      }
    }
  };
}

function businessTripMissingDestinationResponse(event: NormalizedTravelEvent, timezone: string): StructuredResponse {
  return {
    speech: `提醒你，明天${formatLocalTime(event.start, timezone)}有${event.title}，但日程里没有明确城市或目的地，请提前确认行程信息。`,
    presentation: {
      emotion: "thinking",
      motion: "look_at_user",
      light: "amber"
    },
    actions: [
      {
        type: "weather.forecast",
        status: "skipped",
        details: {
          reason: "missing_destination",
          event_id: event.id,
          title: event.title,
          trigger_key: event.triggerKey
        }
      },
      {
        type: "travel.plan.generate",
        status: "failed",
        error: {
          code: "MISSING_DESTINATION",
          message: "No usable destination was resolved from calendar location, description, or title."
        },
        details: {
          travel_type: "business_trip_tomorrow",
          event_id: event.id,
          title: event.title,
          start: event.start,
          trigger_key: event.triggerKey
        }
      }
    ],
    follow_up: {
      expected: false
    },
    context_patch: {
      travel_summary: {
        source_event_id: event.id,
        title: event.title,
        trip_date: localDate(event.start)
      }
    }
  };
}

function buildOutdoorSpeech(
  event: NormalizedTravelEvent,
  destination: string,
  timezone: string,
  route: RouteEstimate | undefined,
  recommendedDepartureTime: string | undefined
): string {
  const time = formatLocalTime(event.start, timezone);
  const descriptionNote = extractPreparationNote(event.description);
  const routeText =
    route && recommendedDepartureTime
      ? `路上预计 ${route.durationMinutes} 分钟，建议 ${formatLocalTime(recommendedDepartureTime, timezone)} 出发。`
      : "我还不能给出精确出发时间，请按实际路况预留路上时间。";
  const note = descriptionNote ? `记得${descriptionNote}。` : "记得带好相关材料。";
  return `提醒你，${time}有${event.title}，目的地是${destination}。${routeText}${note}`;
}

function buildBusinessTripSpeech(
  event: NormalizedTravelEvent,
  destination: string,
  timezone: string,
  forecast: WeatherForecast | undefined
): string {
  const time = formatLocalTime(event.start, timezone);
  const weatherText = forecast ? `${forecast.summary}` : "我暂时没有查到可用天气。";
  const notes = buildPreparationNotes(event, forecast);
  return `提醒你，明天${time}有${event.title}，目的地是${destination}。${weatherText}建议准备${notes.join("、")}。`;
}

function buildPreparationNotes(event: NormalizedTravelEvent, forecast: WeatherForecast | undefined): string[] {
  const notes = ["证件", "充电器", "工作材料"];
  const weatherNote = weatherPreparationNote(forecast);
  if (weatherNote) notes.push(weatherNote);
  const descriptionNote = extractPreparationNote(event.description);
  if (descriptionNote) notes.push(descriptionNote);
  return notes.slice(0, 5);
}

function weatherPreparationNote(forecast: WeatherForecast | undefined): string | undefined {
  if (!forecast) return undefined;
  if (forecast.condition === "rain" || (forecast.precipitationChance ?? 0) >= 50) return "雨具";
  if (forecast.condition === "cold" || (forecast.lowCelsius ?? 99) <= 8) return "外套";
  if (forecast.condition === "hot" || (forecast.highCelsius ?? 0) >= 32) return "轻便衣物";
  if (
    forecast.lowCelsius !== undefined &&
    forecast.highCelsius !== undefined &&
    forecast.highCelsius - forecast.lowCelsius >= 12
  ) {
    return "便于增减的衣物";
  }
  return undefined;
}

function extractPreparationNote(description: string | undefined): string | undefined {
  if (!description) return undefined;
  const sentences = description
    .split(/[。；;\n]/)
    .map((item) => item.trim())
    .filter(Boolean);
  const sentence = sentences.find((item) => /材料|文件|证件|着装|正装|合同|方案|电脑|会议/.test(item));
  return sentence ? sentence.slice(0, 40) : undefined;
}

function skippedRouteAction(
  event: NormalizedTravelEvent,
  destination: DestinationSummary,
  code: string,
  message: string
): ToolAction {
  return {
    type: "route.estimate",
    status: "skipped",
    error: {
      code,
      message
    },
    details: {
      event_id: event.id,
      title: event.title,
      destination: destination.destination,
      destination_source: destination.source,
      trigger_key: event.triggerKey
    }
  };
}

function routeActionDetails(
  event: NormalizedTravelEvent,
  destination: DestinationSummary,
  preferences: TravelPreferences
): Record<string, unknown> {
  return {
    event_id: event.id,
    title: event.title,
    origin: preferences.originAddress,
    destination: destination.destination,
    destination_source: destination.source,
    route_mode: preferences.routeMode,
    arrival_buffer_minutes: preferences.arrivalBufferMinutes,
    trigger_key: event.triggerKey
  };
}

function weatherActionDetails(event: NormalizedTravelEvent, destination: DestinationSummary): Record<string, unknown> {
  return {
    event_id: event.id,
    title: event.title,
    destination: destination.destination,
    destination_source: destination.source,
    date: localDate(event.start),
    trigger_key: event.triggerKey
  };
}

function buildTravelFocus(
  event: NormalizedTravelEvent,
  destination: string,
  recommendedDepartureTime: string | undefined
): Record<string, unknown> {
  const focus: Record<string, unknown> = {
    type: "travel_event",
    event_id: event.id,
    title: event.title,
    start_time: event.start,
    end_time: event.end,
    destination
  };
  if (event.calendarId) focus.calendar_id = event.calendarId;
  if (recommendedDepartureTime) focus.recommended_departure_time = recommendedDepartureTime;
  return focus;
}

function calculateDepartureTime(start: string, durationMinutes: number, bufferMinutes: number): string | undefined {
  const startMs = Date.parse(start);
  if (!Number.isFinite(startMs)) return undefined;
  return new Date(startMs - (durationMinutes + bufferMinutes) * 60_000).toISOString();
}

function formatLocalTime(value: string, timezone: string): string {
  const date = new Date(value);
  if (!Number.isFinite(date.getTime())) return value;
  try {
    return new Intl.DateTimeFormat("zh-CN", {
      timeZone: timezone,
      hour: "2-digit",
      minute: "2-digit",
      hour12: false
    }).format(date);
  } catch {
    return value;
  }
}

function localDate(value: string): string {
  const date = new Date(value);
  if (!Number.isFinite(date.getTime())) return value.slice(0, 10);
  return date.toISOString().slice(0, 10);
}

function mergeProfile(config: TravelPlannerConfig, profile: UserProfile): UserProfile {
  const merged: UserProfile = {};
  if (profile.originAddress) merged.originAddress = profile.originAddress;
  if (config.originAddress) merged.originAddress = config.originAddress;
  if (profile.homeCity) merged.homeCity = profile.homeCity;
  if (profile.defaultRouteMode) merged.defaultRouteMode = profile.defaultRouteMode;
  if (config.defaultRouteMode) merged.defaultRouteMode = config.defaultRouteMode;
  if (profile.arrivalBufferMinutes) merged.arrivalBufferMinutes = profile.arrivalBufferMinutes;
  if (config.arrivalBufferMinutes) merged.arrivalBufferMinutes = config.arrivalBufferMinutes;
  return merged;
}

function readString(value: unknown): string | undefined {
  return typeof value === "string" && value.trim() !== "" ? value.trim() : undefined;
}

function isValidDate(value: string): boolean {
  return Number.isFinite(Date.parse(value));
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function safeErrorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}
