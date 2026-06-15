import type { InputEvent, SchedulerCalendarEventPayload, SchedulerTriggerPayload, StructuredResponse } from "../contracts.js";
import type { LarkCalendarAdapter } from "../lark/adapters.js";
import { calculateScanWindow } from "./time.js";
import { generateTriggerPlans } from "./rules.js";
import type {
  ProactiveCalendarDispatchResult,
  ProactiveCalendarScanResult,
  ProactiveCalendarSchedulerConfig,
  SchedulerDispatchCallback,
  SchedulerDispatchCallbackResult,
  TriggerPlanStore,
  TriggerStoreRecord
} from "./types.js";

export type ProactiveCalendarTriggerSchedulerOptions = {
  config: ProactiveCalendarSchedulerConfig;
  calendarAdapter: LarkCalendarAdapter;
  store: TriggerPlanStore;
  dispatch: SchedulerDispatchCallback;
};

export class ProactiveCalendarTriggerScheduler {
  private readonly config: ProactiveCalendarSchedulerConfig;
  private readonly calendarAdapter: LarkCalendarAdapter;
  private readonly store: TriggerPlanStore;
  private readonly dispatch: SchedulerDispatchCallback;

  constructor(options: ProactiveCalendarTriggerSchedulerOptions) {
    this.config = options.config;
    this.calendarAdapter = options.calendarAdapter;
    this.store = options.store;
    this.dispatch = options.dispatch;
  }

  async refresh(now = new Date()): Promise<ProactiveCalendarScanResult> {
    const window = calculateScanWindow(now, this.config.lookaheadHours);
    const scanWindow = {
      start: window.start,
      end: window.end,
      timezone: this.config.timezone
    };
    const result = await this.calendarAdapter.listEvents({
      start: window.start,
      end: window.end,
      calendarId: this.config.calendarId
    });
    if (!result.ok) {
      return {
        ok: false,
        type: "proactive.calendar.scan",
        window: scanWindow,
        calendarId: this.config.calendarId,
        code: result.code,
        message: result.message
      };
    }

    const plans = generateTriggerPlans({
      now,
      timezone: this.config.timezone,
      userId: this.config.userId,
      calendarId: result.calendarId ?? this.config.calendarId,
      maxDispatchAttempts: this.config.maxDispatchAttempts,
      rules: this.config.rules,
      events: result.events
    });
    const upsert = await this.store.upsertPlans(plans, now.toISOString());
    return {
      ok: true,
      type: "proactive.calendar.scan",
      window: scanWindow,
      calendarId: result.calendarId ?? this.config.calendarId,
      eventCount: result.events.length,
      planCount: plans.length,
      upserted: upsert.upserted,
      replacedPending: upsert.replacedPending
    };
  }

  async dispatchDue(now = new Date()): Promise<ProactiveCalendarDispatchResult[]> {
    const firedAt = now.toISOString();
    const due = await this.store.getDue(firedAt);
    const results: ProactiveCalendarDispatchResult[] = [];
    for (const record of due) {
      const event = this.toInputEvent(record, firedAt);
      try {
        const dispatchResult = await this.dispatch(event);
        const response = unwrapDispatchResponse(dispatchResult);
        if (!response.ok) {
          await this.store.recordDispatchFailure(record.key, response.message, firedAt);
          results.push({
            key: record.key,
            eventId: record.eventId,
            type: record.type,
            status: "failed",
            error: response.message
          });
          continue;
        }
        await this.store.markDispatched(record.key, record.eventId, firedAt, response.response);
        results.push({
          key: record.key,
          eventId: record.eventId,
          type: record.type,
          status: "success"
        });
      } catch (error) {
        const message = error instanceof Error ? error.message : String(error);
        await this.store.recordDispatchFailure(record.key, message, firedAt);
        results.push({
          key: record.key,
          eventId: record.eventId,
          type: record.type,
          status: "failed",
          error: message
        });
      }
    }
    return results;
  }

  async tick(now = new Date()): Promise<{ scan: ProactiveCalendarScanResult; dispatches: ProactiveCalendarDispatchResult[] }> {
    const scan = await this.refresh(now);
    const dispatches = await this.dispatchDue(now);
    return { scan, dispatches };
  }

  async listRecords(): Promise<TriggerStoreRecord[]> {
    return this.store.listRecords();
  }

  private toInputEvent(record: TriggerStoreRecord, firedAt: string): InputEvent {
    const trigger: SchedulerTriggerPayload = {
      rule_id: record.ruleId,
      scheduled_for: record.scheduledFor,
      fired_at: firedAt,
      source: "proactive_calendar_scheduler",
      trigger_key: record.key,
      calendar_id: record.calendarId,
      ...(record.sourceEventId ? { source_event_id: record.sourceEventId } : {})
    };
    const payload: InputEvent["payload"] = { trigger };
    if (record.calendarEvent) {
      const calendarEvent: SchedulerCalendarEventPayload = {
        id: record.calendarEvent.id,
        title: record.calendarEvent.title,
        start: record.calendarEvent.start,
        end: record.calendarEvent.end,
        calendar_id: record.calendarEvent.calendarId ?? record.calendarId,
        ...(record.calendarEvent.location ? { location: record.calendarEvent.location } : {}),
        ...(record.calendarEvent.description ? { description: record.calendarEvent.description } : {}),
        ...(record.calendarEvent.notificationTarget
          ? {
              notification_target: {
                ...(record.calendarEvent.notificationTarget.chatId ? { chat_id: record.calendarEvent.notificationTarget.chatId } : {}),
                ...(record.calendarEvent.notificationTarget.attendeeUserIds
                  ? { attendee_user_ids: record.calendarEvent.notificationTarget.attendeeUserIds }
                  : {})
              }
            }
          : {})
      };
      payload.calendar_event = calendarEvent;
    }
    return {
      event_id: record.eventId,
      type: record.type,
      timestamp: firedAt,
      user_id: record.userId,
      payload,
      context: {
        timezone: this.config.timezone
      }
    };
  }
}

export function unwrapDispatchResponse(
  result: StructuredResponse | SchedulerDispatchCallbackResult
): SchedulerDispatchCallbackResult {
  if (typeof result === "object" && result !== null && "ok" in result && typeof result.ok === "boolean") {
    return result;
  }
  return {
    ok: true,
    response: result
  };
}
