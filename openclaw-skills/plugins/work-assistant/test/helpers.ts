import { AgendaBriefingAssistant } from "../src/agenda/assistant.js";
import { CalendarAssistant } from "../src/calendar/assistant.js";
import { createWorkAssistantHandler } from "../src/handler.js";
import { MeetingReminderAssistant } from "../src/meeting/assistant.js";
import { TravelPlannerAssistant } from "../src/travel/assistant.js";
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
} from "../src/travel/adapters.js";
import { WellbeingCompanionAssistant } from "../src/wellbeing/assistant.js";
import type {
  LarkCalendarAdapter,
  LarkCalendarCreateRequest,
  LarkCalendarCreateResult,
  LarkCalendarListRequest,
  LarkCalendarListResult,
  LarkContactAdapter,
  LarkIMAdapter,
  LarkMessageSendRequest,
  LarkMessageSendResult,
  NormalizedAgendaEvent,
  LarkPersonResolution
} from "../src/lark/adapters.js";

export const sampleEvent = {
  event_id: "evt-1",
  type: "user_utterance",
  timestamp: "2026-06-05T10:00:00+08:00",
  user_id: "ou_requester",
  payload: {
    text: "明天上午10点到11点的项目会，帮我建一个飞书日程，邀请张三、李四参会。"
  },
  context: {
    timezone: "Asia/Shanghai"
  }
};

export class FakeContactAdapter implements LarkContactAdapter {
  public calls: string[][] = [];

  constructor(private readonly resolutions: Record<string, LarkPersonResolution>) {}

  async resolvePeople(names: string[]): Promise<Record<string, LarkPersonResolution>> {
    this.calls.push(names);
    const result: Record<string, LarkPersonResolution> = {};
    for (const name of names) {
      result[name] = this.resolutions[name] ?? { status: "missing" };
    }
    return result;
  }
}

export class FakeCalendarAdapter implements LarkCalendarAdapter {
  public calls: LarkCalendarCreateRequest[] = [];
  public listCalls: LarkCalendarListRequest[] = [];

  constructor(
    private readonly result: LarkCalendarCreateResult = { ok: true, eventId: "evt_lark_1", calendarId: "primary" },
    private readonly listResult:
      | LarkCalendarListResult
      | ((request: LarkCalendarListRequest) => LarkCalendarListResult) = defaultAgendaListResult
  ) {}

  async createEvent(request: LarkCalendarCreateRequest): Promise<LarkCalendarCreateResult> {
    this.calls.push(request);
    return this.result;
  }

  async listEvents(request: LarkCalendarListRequest): Promise<LarkCalendarListResult> {
    this.listCalls.push(request);
    return typeof this.listResult === "function" ? this.listResult(request) : this.listResult;
  }
}

export class FakeIMAdapter implements LarkIMAdapter {
  public calls: LarkMessageSendRequest[] = [];

  constructor(private readonly result: LarkMessageSendResult = { ok: true, messageId: "om_lark_1" }) {}

  async sendText(request: LarkMessageSendRequest): Promise<LarkMessageSendResult> {
    this.calls.push(request);
    return this.result;
  }
}

export class FakeRouteAdapter implements RouteAdapter {
  public calls: RouteEstimateRequest[] = [];

  constructor(private readonly result: RouteEstimateResult = defaultRouteResult) {}

  async estimateRoute(request: RouteEstimateRequest): Promise<RouteEstimateResult> {
    this.calls.push(request);
    return this.result;
  }
}

export class FakeWeatherAdapter implements WeatherAdapter {
  public calls: WeatherForecastRequest[] = [];

  constructor(private readonly result: WeatherForecastResult = defaultWeatherResult) {}

  async getForecast(request: WeatherForecastRequest): Promise<WeatherForecastResult> {
    this.calls.push(request);
    return this.result;
  }
}

export class FakeUserProfileAdapter implements UserProfileAdapter {
  public calls: UserProfileReadRequest[] = [];

  constructor(private readonly result: UserProfileReadResult = defaultProfileResult) {}

  async readProfile(request: UserProfileReadRequest): Promise<UserProfileReadResult> {
    this.calls.push(request);
    return this.result;
  }
}

export function createTestHandler(options?: {
  contact?: LarkContactAdapter;
  calendar?: FakeCalendarAdapter;
  im?: FakeIMAdapter;
  route?: FakeRouteAdapter;
  weather?: FakeWeatherAdapter;
  profile?: FakeUserProfileAdapter;
  travelConfig?: TravelPlannerConfig;
}) {
  const calendar = options?.calendar ?? new FakeCalendarAdapter();
  const im = options?.im ?? new FakeIMAdapter();
  const route = options?.route ?? new FakeRouteAdapter();
  const weather = options?.weather ?? new FakeWeatherAdapter();
  const profile = options?.profile ?? new FakeUserProfileAdapter();
  const contact =
    options?.contact ??
    new FakeContactAdapter({
      张三: {
        status: "unique",
        person: { id: "ou_zhangsan", name: "张三" }
      },
      李四: {
        status: "unique",
        person: { id: "ou_lisi", name: "李四" }
      }
    });
  return {
    calendar,
    handler: createWorkAssistantHandler({
      calendarAssistant: new CalendarAssistant({
        contactAdapter: contact,
        calendarAdapter: calendar
      }),
      agendaBriefingAssistant: new AgendaBriefingAssistant({
        calendarAdapter: calendar
      }),
      meetingReminderAssistant: new MeetingReminderAssistant({
        imAdapter: im
      }),
      travelPlannerAssistant: new TravelPlannerAssistant({
        routeAdapter: route,
        weatherAdapter: weather,
        userProfileAdapter: profile,
        ...(options?.travelConfig ?? {})
      }),
      wellbeingCompanionAssistant: new WellbeingCompanionAssistant({
        calendarAdapter: calendar
      })
    }),
    im,
    route,
    weather,
    profile
  };
}

const defaultRouteResult: RouteEstimateResult = {
  ok: true,
  estimate: {
    origin: "上海办公室",
    destination: "客户园区",
    durationMinutes: 42,
    distanceMeters: 18500,
    mode: "driving",
    provider: "fake"
  }
};

const defaultWeatherResult: WeatherForecastResult = {
  ok: true,
  forecast: {
    location: "北京",
    date: "2026-06-07",
    summary: "北京明天多云，18 到 27 度。",
    condition: "cloudy",
    lowCelsius: 18,
    highCelsius: 27,
    precipitationChance: 20,
    provider: "fake"
  }
};

const defaultProfileResult: UserProfileReadResult = {
  ok: true,
  profile: {
    originAddress: "上海办公室",
    defaultRouteMode: "driving",
    arrivalBufferMinutes: 15
  }
};

export function defaultAgendaListResult(request: LarkCalendarListRequest): LarkCalendarListResult {
  return {
    ok: true,
    calendarId: request.calendarId ?? "primary",
    events: request.end <= "2026-05-31T00:00:00.000Z" ? sampleRecapEvents : sampleTodayEvents
  };
}

export const sampleTodayEvents: NormalizedAgendaEvent[] = [
  {
    id: "today_customer",
    title: "客户来访接待",
    start: "2026-06-06T09:30:00+08:00",
    end: "2026-06-06T10:30:00+08:00",
    location: "上海办公室",
    description: "准备客户方案材料",
    attendeeCount: 5,
    notificationTarget: {
      chatId: "oc_customer_visit"
    }
  },
  {
    id: "today_internal",
    title: "项目内部同步",
    start: "2026-06-06T14:00:00+08:00",
    end: "2026-06-06T14:30:00+08:00",
    attendeeCount: 3
  },
  {
    id: "today_focus",
    title: "专注写作方案",
    start: "2026-06-06T16:00:00+08:00",
    end: "2026-06-06T17:00:00+08:00",
    attendeeCount: 1
  }
];

export const sampleRecapEvents: NormalizedAgendaEvent[] = [
  {
    id: "recap_internal",
    title: "内部周会",
    start: "2026-05-25T10:00:00+08:00",
    end: "2026-05-25T11:00:00+08:00",
    attendeeCount: 8
  },
  {
    id: "recap_customer",
    title: "客户方案评审",
    start: "2026-05-27T15:00:00+08:00",
    end: "2026-05-27T16:00:00+08:00",
    attendeeCount: 6
  },
  {
    id: "recap_outdoor",
    title: "外出拜访",
    start: "2026-05-29T13:00:00+08:00",
    end: "2026-05-29T15:00:00+08:00",
    location: "客户园区"
  }
];
