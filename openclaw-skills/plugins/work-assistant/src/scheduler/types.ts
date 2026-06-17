import type { InputEvent, StructuredResponse } from "../contracts.js";
import type { NormalizedAgendaEvent } from "../lark/adapters.js";

export type ProactiveCalendarRuleId =
  | "daily_briefing"
  | "meeting_starting_soon"
  | "outdoor_event"
  | "business_trip_tomorrow";

export type ProactiveCalendarTriggerType =
  | "daily_briefing_triggered"
  | "meeting_starting_soon"
  | "outdoor_event_detected"
  | "business_trip_tomorrow_detected";

export type RuleEnablement = {
  enabled: boolean;
};

export type DailyBriefingRuleConfig = RuleEnablement & {
  localTime: string;
};

export type MeetingStartingSoonRuleConfig = RuleEnablement & {
  offsetMinutes: number;
  keywords: string[];
};

export type OutdoorEventRuleConfig = RuleEnablement & {
  offsetMinutes: number;
  keywords: string[];
};

export type BusinessTripTomorrowRuleConfig = RuleEnablement & {
  localTime: string;
  keywords: string[];
};

export type ProactiveCalendarRuleConfig = {
  dailyBriefing: DailyBriefingRuleConfig;
  meetingStartingSoon: MeetingStartingSoonRuleConfig;
  outdoorEvent: OutdoorEventRuleConfig;
  businessTripTomorrow: BusinessTripTomorrowRuleConfig;
};

export type ProactiveCalendarSchedulerConfig = {
  enabled: boolean;
  startIntervalLoop: boolean;
  scanIntervalMs: number;
  lookaheadHours: number;
  timezone: string;
  userId: string;
  calendarId: string;
  statePath?: string;
  maxDispatchAttempts: number;
  agentDispatch: ProactiveCalendarAgentDispatchConfig;
  rules: ProactiveCalendarRuleConfig;
};

export type ProactiveCalendarAgentDispatchConfig = {
  enabled: boolean;
  sessionKey?: string;
  sessionKeyMode?: "static" | "online_xiaopai";
  agentId?: string;
  deliveryMode: "none" | "announce";
  deviceId?: string;
  xiaopaiBaseUrl?: string;
  xiaopaiDeviceLookupTimeoutMs?: number;
  interrupt: boolean;
};

export type TriggerCalendarEventSummary = {
  id: string;
  title: string;
  start: string;
  end: string;
  calendarId?: string;
  location?: string;
  description?: string;
  notificationTarget?: {
    chatId?: string;
    attendeeUserIds?: string[];
  };
};

export type TriggerPlan = {
  key: string;
  updateGroupKey: string;
  eventId: string;
  ruleId: ProactiveCalendarRuleId;
  type: ProactiveCalendarTriggerType;
  userId: string;
  calendarId: string;
  scheduledFor: string;
  eventHash: string;
  maxAttempts: number;
  sourceEventId?: string;
  calendarEvent?: TriggerCalendarEventSummary;
};

export type TriggerStoreRecord = TriggerPlan & {
  status: "pending" | "dispatched";
  createdAt: string;
  updatedAt: string;
  attempts: number;
  dispatchedAt?: string;
  lastEventId?: string;
  lastDispatchError?: string;
  responseSummary?: {
    speech?: string;
    actionCount?: number;
    followUpExpected?: boolean;
  };
};

export type TriggerStoreSnapshot = {
  version: 1;
  records: TriggerStoreRecord[];
};

export type TriggerUpsertResult = {
  upserted: number;
  replacedPending: number;
};

export interface TriggerPlanStore {
  upsertPlans(plans: TriggerPlan[], now: string): Promise<TriggerUpsertResult>;
  getDue(now: string): Promise<TriggerStoreRecord[]>;
  markDispatched(key: string, eventId: string, dispatchedAt: string, response: StructuredResponse): Promise<void>;
  recordDispatchFailure(key: string, error: string, failedAt: string): Promise<void>;
  listRecords(): Promise<TriggerStoreRecord[]>;
}

export type ProactiveCalendarScanResult =
  | {
      ok: true;
      type: "proactive.calendar.scan";
      window: {
        start: string;
        end: string;
        timezone: string;
      };
      calendarId: string;
      eventCount: number;
      planCount: number;
      upserted: number;
      replacedPending: number;
    }
  | {
      ok: false;
      type: "proactive.calendar.scan";
      window: {
        start: string;
        end: string;
        timezone: string;
      };
      calendarId: string;
      code: string;
      message: string;
    };

export type ProactiveCalendarDispatchResult = {
  key: string;
  eventId: string;
  type: ProactiveCalendarTriggerType;
  status: "success" | "failed" | "skipped";
  error?: string;
};

export type SchedulerDispatchCallbackResult =
  | { ok: true; response: StructuredResponse }
  | { ok: false; code?: string; message: string };

export type SchedulerDispatchCallback = (
  event: InputEvent
) => Promise<StructuredResponse | SchedulerDispatchCallbackResult>;

export type TriggerRuleContext = {
  now: Date;
  timezone: string;
  userId: string;
  calendarId: string;
  maxDispatchAttempts: number;
  rules: ProactiveCalendarRuleConfig;
};

export type TriggerRuleInput = TriggerRuleContext & {
  events: NormalizedAgendaEvent[];
};
