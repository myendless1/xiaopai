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
  LarkPersonResolution
} from "./adapters.js";

export class DryRunContactAdapter implements LarkContactAdapter {
  async resolvePeople(names: string[]): Promise<Record<string, LarkPersonResolution>> {
    const result: Record<string, LarkPersonResolution> = {};
    for (const name of names) {
      result[name] = {
        status: "unique",
        person: {
          id: `ou_dry_run_${encodeURIComponent(name)}`,
          name
        }
      };
    }
    return result;
  }
}

export class DryRunCalendarAdapter implements LarkCalendarAdapter {
  async createEvent(request: LarkCalendarCreateRequest): Promise<LarkCalendarCreateResult> {
    return {
      ok: true,
      eventId: `dry_run_${Buffer.from(`${request.title}:${request.start}`).toString("base64url")}`,
      calendarId: "primary",
      link: "dry-run://work-assistant/calendar-event"
    };
  }

  async listEvents(request: LarkCalendarListRequest): Promise<LarkCalendarListResult> {
    const events = request.end <= "2026-06-06T00:00:00.000Z" ? dryRunRecapEvents() : dryRunTodayEvents();
    return {
      ok: true,
      calendarId: request.calendarId ?? "primary",
      events
    };
  }
}

export class DryRunIMAdapter implements LarkIMAdapter {
  public readonly sentMessages: LarkMessageSendRequest[] = [];

  async sendText(request: LarkMessageSendRequest): Promise<LarkMessageSendResult> {
    this.sentMessages.push(request);
    const target = request.chatId ?? request.attendeeUserIds?.join(",") ?? "missing_target";
    const messageId = `dry_msg_${Buffer.from(`${target}:${request.text}`).toString("base64url").slice(0, 48)}`;
    const result: Extract<LarkMessageSendResult, { ok: true }> = {
      ok: true,
      messageId
    };
    if (request.chatId) result.chatId = request.chatId;
    if (request.attendeeUserIds && request.attendeeUserIds.length > 0) {
      result.attendeeUserIds = request.attendeeUserIds;
    }
    return result;
  }
}

function dryRunTodayEvents() {
  return [
    {
      id: "dry_today_1",
      title: "客户来访接待",
      start: "2026-06-06T09:30:00+08:00",
      end: "2026-06-06T10:30:00+08:00",
      location: "上海办公室",
      description: "准备客户方案材料",
      attendeeCount: 5,
      notificationTarget: {
        chatId: "oc_dry_run_customer_visit"
      }
    },
    {
      id: "dry_today_2",
      title: "项目内部同步",
      start: "2026-06-06T14:00:00+08:00",
      end: "2026-06-06T14:30:00+08:00",
      location: "线上会议",
      attendeeCount: 4,
      notificationTarget: {
        attendeeUserIds: ["ou_dry_run_zhangsan", "ou_dry_run_lisi"]
      }
    },
    {
      id: "dry_today_3",
      title: "专注写作方案",
      start: "2026-06-06T16:00:00+08:00",
      end: "2026-06-06T17:00:00+08:00",
      attendeeCount: 1
    },
    {
      id: "dry_today_4",
      title: "外出客户园区拜访",
      start: "2026-06-06T18:00:00+08:00",
      end: "2026-06-06T19:30:00+08:00",
      location: "客户园区",
      description: "带方案材料到现场沟通"
    },
    {
      id: "dry_tomorrow_trip",
      title: "北京出差",
      start: "2026-06-07T09:00:00+08:00",
      end: "2026-06-07T20:00:00+08:00",
      location: "北京",
      description: "航班和客户会议"
    }
  ];
}

function dryRunRecapEvents() {
  return [
    {
      id: "dry_recap_1",
      title: "内部周会",
      start: "2026-06-01T10:00:00+08:00",
      end: "2026-06-01T11:00:00+08:00",
      attendeeCount: 8
    },
    {
      id: "dry_recap_2",
      title: "客户方案评审",
      start: "2026-06-03T15:00:00+08:00",
      end: "2026-06-03T16:00:00+08:00",
      location: "会议室 A",
      attendeeCount: 6
    },
    {
      id: "dry_recap_3",
      title: "外出拜访",
      start: "2026-06-05T13:00:00+08:00",
      end: "2026-06-05T15:00:00+08:00",
      location: "客户园区"
    }
  ];
}
