import type {
  CurrentMeetingFocus,
  InputEvent,
  MeetingNotificationTarget,
  SchedulerCalendarEventPayload,
  StructuredMeetingNotifyLateIntent,
  StructuredResponse,
  ToolAction
} from "../contracts.js";
import { followUpResponse, validateStructuredAssistantIntent } from "../contracts.js";
import type { LarkIMAdapter, LarkMessageSendRequest } from "../lark/adapters.js";

export type MeetingReminderAssistantOptions = {
  imAdapter: LarkIMAdapter;
};

type LateNotificationRequest = {
  delayMinutes?: number;
  message?: string;
  source: "structured" | "fallback";
};

export class MeetingReminderAssistant {
  private readonly imAdapter: LarkIMAdapter;

  constructor(options: MeetingReminderAssistantOptions) {
    this.imAdapter = options.imAdapter;
  }

  async handleReminder(event: InputEvent): Promise<StructuredResponse> {
    const calendarEvent = normalizeCalendarEvent(event.payload.calendar_event);
    if (!calendarEvent) {
      return {
        speech: "会议提醒数据不完整，我暂时不能生成这条提醒。",
        presentation: {
          emotion: "concerned",
          motion: "none",
          light: "amber"
        },
        actions: [
          {
            type: "meeting.reminder.generate",
            status: "failed",
            error: {
              code: "MALFORMED_MEETING_REMINDER_EVENT",
              message: "payload.calendar_event must include id, title, start, and end."
            }
          }
        ],
        follow_up: {
          expected: false
        },
        context_patch: {}
      };
    }

    const action: ToolAction = {
      type: "meeting.reminder.generate",
      status: "success",
      details: {
        event_id: calendarEvent.id,
        calendar_id: calendarEvent.calendar_id ?? "primary",
        title: calendarEvent.title,
        start: calendarEvent.start,
        end: calendarEvent.end,
        location: calendarEvent.location,
        trigger_key: readTriggerKey(event.payload.trigger)
      }
    };

    return {
      speech: buildReminderSpeech(calendarEvent, event.context.timezone),
      presentation: {
        emotion: "focused",
        motion: "look_at_user",
        light: "soft_blink"
      },
      actions: [action],
      follow_up: {
        expected: false
      },
      context_patch: {
        current_focus: buildCurrentFocus(calendarEvent)
      }
    };
  }

  async handleLateNotification(event: InputEvent): Promise<StructuredResponse> {
    const notification = normalizeLateNotificationRequest(event);
    if ("response" in notification) return notification.response;

    const focus = normalizeCurrentMeetingFocus(event.context.current_focus);
    if (!focus) {
      return followUpResponse(
        "要通知哪一个会议的参会人？",
        "missing_meeting_focus",
        "请告诉我要通知哪一个会议。"
      );
    }

    const target = normalizeNotificationTarget(focus.notification_target);
    if (!target) {
      return {
        speech: "我还不知道要通知哪个会议群或哪些参会人。",
        presentation: {
          emotion: "thinking",
          motion: "look_at_user",
          light: "soft_blink"
        },
        actions: [],
        follow_up: {
          expected: true,
          question: "请告诉我要发到哪个会议群，或要通知哪些参会人。",
          reason: "missing_notification_target"
        },
        context_patch: {
          current_focus: focus
        }
      };
    }

    const text = formatLateNotificationMessage(notification.intent, focus);
    const request: LarkMessageSendRequest = {
      text,
      requesterId: event.user_id,
      idempotencyKey: event.event_id
    };
    if (target.chat_id) request.chatId = target.chat_id;
    if (target.attendee_user_ids && target.attendee_user_ids.length > 0) {
      request.attendeeUserIds = target.attendee_user_ids;
    }

    const result = await this.imAdapter.sendText(request);
    if (!result.ok) {
      return {
        speech: "我没能把迟到通知发出去，已经记录了失败原因。",
        presentation: {
          emotion: "concerned",
          motion: "look_at_user",
          light: "amber"
        },
        actions: [
          {
            type: "lark.message.send",
            status: "failed",
            error: {
              code: result.code,
              message: result.message
            },
            details: {
              target: safeTargetDetails(target),
              meeting_event_id: focus.event_id
            }
          }
        ],
        follow_up: {
          expected: false
        },
        context_patch: {
          current_focus: focus
        }
      };
    }

    return {
      speech: "已通知参会人你会晚到。",
      presentation: {
        emotion: "happy",
        motion: "nod",
        light: "blink"
      },
      actions: [
        {
          type: "lark.message.send",
          status: "success",
          resource_id: result.messageId,
          details: {
            target: safeTargetDetails(target),
            meeting_event_id: focus.event_id,
            delay_minutes: notification.intent.delayMinutes,
            source: notification.intent.source
          }
        }
      ],
      follow_up: {
        expected: false
      },
      context_patch: {
        current_focus: focus
      }
    };
  }
}

export function shouldRouteToMeetingNotification(event: InputEvent): boolean {
  if (event.type !== "user_utterance") return false;
  const structuredIntent = event.payload.structured_intent;
  if (isRecord(structuredIntent) && structuredIntent.type === "meeting.notify_late") return true;
  const text = typeof event.payload.text === "string" ? event.payload.text : "";
  if (!text) return false;
  return looksLikeLateNotification(text);
}

function normalizeLateNotificationRequest(
  event: InputEvent
): { intent: LateNotificationRequest } | { response: StructuredResponse } {
  if (Object.prototype.hasOwnProperty.call(event.payload, "structured_intent")) {
    const validation = validateStructuredAssistantIntent(event.payload.structured_intent);
    if (!validation.ok) {
      return {
        response: followUpResponse(
          validation.reason === "unsupported_intent_version"
            ? "这个迟到通知请求版本暂不支持，请使用 version 为 1 的格式。"
            : "迟到通知请求格式无效，请补充后再试。",
          validation.reason,
          "结构化迟到通知请求不完整或无效，请补充后再试。"
        )
      };
    }
    if (validation.value.type !== "meeting.notify_late") {
      return {
        response: followUpResponse("这个结构化请求不是会议通知请求。", "unsupported_intent_type")
      };
    }
    return {
      intent: fromStructuredIntent(validation.value)
    };
  }

  const text = typeof event.payload.text === "string" ? event.payload.text : "";
  const fallback = parseLateNotificationFallback(text);
  if (!fallback) {
    return {
      response: followUpResponse("请说明要通知参会人你会晚到多久。", "unsupported_meeting_notification")
    };
  }
  return {
    intent: fallback
  };
}

function fromStructuredIntent(intent: StructuredMeetingNotifyLateIntent): LateNotificationRequest {
  const result: LateNotificationRequest = { source: "structured" };
  if (intent.delay_minutes !== undefined) result.delayMinutes = intent.delay_minutes;
  if (intent.message) result.message = intent.message;
  return result;
}

function parseLateNotificationFallback(text: string): LateNotificationRequest | undefined {
  if (!looksLikeLateNotification(text)) return undefined;
  const delayMinutes = extractDelayMinutes(text);
  return {
    source: "fallback",
    ...(delayMinutes !== undefined ? { delayMinutes } : {})
  };
}

function looksLikeLateNotification(text: string): boolean {
  const normalized = text.toLocaleLowerCase();
  const asksToNotify = /通知|告知|告诉|发.{0,4}消息|notify|message/.test(normalized);
  const saysLate = /迟到|晚到|来晚|晚点|late|delay/.test(normalized);
  return asksToNotify && saysLate;
}

function extractDelayMinutes(text: string): number | undefined {
  const digitMatch = text.match(/(\d{1,3})\s*(?:分钟|分|min|mins|minute|minutes)/i);
  if (digitMatch?.[1]) return clampDelay(Number(digitMatch[1]));
  const chineseMatch = text.match(/([一二两三四五六七八九十]{1,3})\s*(?:分钟|分)/);
  if (!chineseMatch?.[1]) return undefined;
  return clampDelay(parseChineseNumber(chineseMatch[1]));
}

function clampDelay(value: number): number | undefined {
  return Number.isInteger(value) && value >= 1 && value <= 180 ? value : undefined;
}

function parseChineseNumber(value: string): number {
  const digits: Record<string, number> = {
    一: 1,
    二: 2,
    两: 2,
    三: 3,
    四: 4,
    五: 5,
    六: 6,
    七: 7,
    八: 8,
    九: 9
  };
  if (value === "十") return 10;
  const tenIndex = value.indexOf("十");
  if (tenIndex >= 0) {
    const tens = tenIndex === 0 ? 1 : digits[value.slice(0, tenIndex)] ?? 0;
    const onesText = value.slice(tenIndex + 1);
    const ones = onesText ? digits[onesText] ?? 0 : 0;
    return tens * 10 + ones;
  }
  return digits[value] ?? Number.NaN;
}

function formatLateNotificationMessage(intent: LateNotificationRequest, focus: CurrentMeetingFocus): string {
  if (intent.message) return intent.message.slice(0, 200);
  const delay = intent.delayMinutes ? `我会晚 ${intent.delayMinutes} 分钟到` : "我会晚一点到";
  return `${delay}，请大家先开始或稍等一下。`;
}

function buildReminderSpeech(event: SchedulerCalendarEventPayload, timezone: string): string {
  const time = formatLocalTime(event.start, timezone);
  const location = event.location ? `，地点 ${event.location}` : "";
  return `提醒你，${time}有${event.title}${location}。`;
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

function buildCurrentFocus(event: SchedulerCalendarEventPayload): CurrentMeetingFocus {
  const focus: CurrentMeetingFocus = {
    type: "calendar_event",
    event_id: event.id,
    title: event.title,
    start_time: event.start,
    end_time: event.end
  };
  if (event.calendar_id) focus.calendar_id = event.calendar_id;
  if (event.location) focus.location = event.location;
  const target = normalizeNotificationTarget(
    event.notification_target ?? {
      chat_id: event.chat_id,
      attendee_user_ids: event.attendee_user_ids
    }
  );
  if (target) focus.notification_target = target;
  return focus;
}

function normalizeCurrentMeetingFocus(value: unknown): CurrentMeetingFocus | undefined {
  const record = isRecord(value) ? value : undefined;
  if (!record || record.type !== "calendar_event") return undefined;
  const eventId = readString(record, "event_id");
  const title = readString(record, "title");
  const start = readString(record, "start_time");
  const end = readString(record, "end_time");
  if (!eventId || !title || !start || !end) return undefined;
  const focus: CurrentMeetingFocus = {
    type: "calendar_event",
    event_id: eventId,
    title,
    start_time: start,
    end_time: end
  };
  const calendarId = readString(record, "calendar_id");
  const location = readString(record, "location");
  if (calendarId) focus.calendar_id = calendarId;
  if (location) focus.location = location;
  const target = normalizeNotificationTarget(record.notification_target);
  if (target) focus.notification_target = target;
  return focus;
}

function normalizeCalendarEvent(value: unknown): SchedulerCalendarEventPayload | undefined {
  const record = isRecord(value) ? value : undefined;
  if (!record) return undefined;
  const id = readString(record, "id");
  const title = readString(record, "title");
  const start = readString(record, "start");
  const end = readString(record, "end");
  if (!id || !title || !start || !end) return undefined;
  const event: SchedulerCalendarEventPayload = { id, title, start, end };
  const calendarId = readString(record, "calendar_id") ?? readString(record, "calendarId");
  const location = readString(record, "location");
  const description = readString(record, "description");
  const chatId = readString(record, "chat_id") ?? readString(record, "chatId");
  const attendeeUserIds = readStringArray(record.attendee_user_ids ?? record.attendeeUserIds);
  const target = normalizeNotificationTarget(record.notification_target ?? record.notificationTarget);
  if (calendarId) event.calendar_id = calendarId;
  if (location) event.location = location;
  if (description) event.description = description;
  if (chatId) event.chat_id = chatId;
  if (attendeeUserIds.length > 0) event.attendee_user_ids = attendeeUserIds;
  if (target) event.notification_target = target;
  return event;
}

function normalizeNotificationTarget(value: unknown): MeetingNotificationTarget | undefined {
  const record = isRecord(value) ? value : undefined;
  if (!record) return undefined;
  const chatId = readString(record, "chat_id") ?? readString(record, "chatId");
  const attendeeUserIds = readStringArray(record.attendee_user_ids ?? record.attendeeUserIds);
  const target: MeetingNotificationTarget = {};
  if (chatId) target.chat_id = chatId;
  if (attendeeUserIds.length > 0) target.attendee_user_ids = attendeeUserIds;
  return target.chat_id || target.attendee_user_ids ? target : undefined;
}

function safeTargetDetails(target: MeetingNotificationTarget): Record<string, unknown> {
  return {
    ...(target.chat_id ? { chat_id: target.chat_id } : {}),
    ...(target.attendee_user_ids ? { attendee_count: target.attendee_user_ids.length } : {})
  };
}

function readTriggerKey(value: unknown): string | undefined {
  return isRecord(value) ? readString(value, "trigger_key") : undefined;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function readString(record: Record<string, unknown>, key: string): string | undefined {
  const value = record[key];
  return typeof value === "string" && value.trim() !== "" ? value.trim() : undefined;
}

function readStringArray(value: unknown): string[] {
  if (!Array.isArray(value)) return [];
  return value.flatMap((entry) => (typeof entry === "string" && entry.trim() !== "" ? [entry.trim()] : []));
}
