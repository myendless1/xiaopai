export type SupportedAssistantEventType =
  | "user_utterance"
  | "head_touch"
  | "daily_briefing_triggered"
  | "meeting_starting_soon"
  | "outdoor_event_detected"
  | "business_trip_tomorrow_detected"
  | "sedentary_detected"
  | "wellbeing_companion_requested";

export type AssistantEventType = SupportedAssistantEventType | (string & {});

export type SedentaryDetectedPayload = {
  duration_minutes: number;
  confidence: number;
  source?: string;
  device_id?: string;
  [key: string]: unknown;
};

export type WellbeingCompanionRequestedPayload = {
  request_type?: string;
  content_type?: string;
  continue_requested?: boolean;
  [key: string]: unknown;
};

export type SchedulerTriggerPayload = {
  rule_id: string;
  scheduled_for: string;
  fired_at: string;
  source: "proactive_calendar_scheduler";
  trigger_key: string;
  calendar_id?: string;
  source_event_id?: string;
};

export type SchedulerCalendarEventPayload = {
  id: string;
  title: string;
  start: string;
  end: string;
  calendar_id?: string;
  location?: string;
  description?: string;
  notification_target?: MeetingNotificationTarget;
  chat_id?: string;
  attendee_user_ids?: string[];
};

export type SchedulerProducedPayload = {
  trigger: SchedulerTriggerPayload;
  calendar_event?: SchedulerCalendarEventPayload;
  [key: string]: unknown;
};

export type InputEvent = {
  event_id: string;
  type: AssistantEventType;
  timestamp: string;
  user_id: string;
  payload: {
    /**
     * Original user utterance, retained for audit/debug context and legacy parsing fallback.
     */
    text?: unknown;
    /**
     * Optional deterministic intent extracted by OpenClaw before plugin execution.
     */
    structured_intent?: unknown;
    [key: string]: unknown;
  };
  context: {
    timezone: string;
    [key: string]: unknown;
  };
};

export type StructuredAttendeeReference =
  | {
      name: string;
      id?: never;
    }
  | {
      id: string;
      name?: never;
    };

export type StructuredCalendarCreateIntent = {
  type: "calendar.create";
  version: "1";
  title: string;
  start: string;
  end: string;
  attendees: StructuredAttendeeReference[];
};

export type StructuredMeetingNotifyLateIntent = {
  type: "meeting.notify_late";
  version: "1";
  delay_minutes?: number;
  message?: string;
};

export type StructuredAssistantIntent = StructuredCalendarCreateIntent | StructuredMeetingNotifyLateIntent;

export type MeetingNotificationTarget = {
  chat_id?: string;
  attendee_user_ids?: string[];
};

export type CurrentMeetingFocus = {
  type: "calendar_event";
  event_id: string;
  calendar_id?: string;
  title: string;
  start_time: string;
  end_time: string;
  location?: string;
  notification_target?: MeetingNotificationTarget;
};

export type StructuredIntentValidationReason =
  | "malformed_structured_intent"
  | "unsupported_intent_type"
  | "unsupported_intent_version"
  | "missing_required_structured_fields"
  | "invalid_time_range"
  | "invalid_attendee_reference";

export type ToolAction = {
  type: string;
  status: "success" | "failed" | "skipped";
  resource_id?: string;
  details?: Record<string, unknown>;
  error?: {
    code: string;
    message: string;
  };
};

export type PresentationHints = {
  emotion?: string;
  motion?: string;
  light?: string;
};

export type FollowUp = {
  expected: boolean;
  question?: string;
  reason?: string;
};

export type ContextPatch = Record<string, unknown>;

export type StructuredResponse = {
  speech: string;
  presentation: PresentationHints;
  actions: ToolAction[];
  follow_up: FollowUp;
  context_patch: ContextPatch;
};

export type ValidationResult<T> =
  | { ok: true; value: T }
  | { ok: false; message: string };

export type StructuredIntentValidationResult =
  | { ok: true; value: StructuredAssistantIntent }
  | { ok: false; reason: StructuredIntentValidationReason; message: string };

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function isValidIsoTimestamp(value: string): boolean {
  const parsed = Date.parse(value);
  return Number.isFinite(parsed);
}

function hasValidLarkAttendeeIdPrefix(value: string): boolean {
  return /^(ou_|oc_|omm_).+/.test(value);
}

export function validateInputEvent(value: unknown): ValidationResult<InputEvent> {
  if (!isRecord(value)) return { ok: false, message: "InputEvent must be an object." };
  const eventId = value.event_id;
  const type = value.type;
  const timestamp = value.timestamp;
  const userId = value.user_id;
  const payload = value.payload;
  const context = value.context;

  if (typeof eventId !== "string" || eventId.trim() === "") {
    return { ok: false, message: "InputEvent.event_id must be a non-empty string." };
  }
  if (typeof type !== "string" || type.trim() === "") {
    return { ok: false, message: "InputEvent.type must be a non-empty string." };
  }
  if (typeof timestamp !== "string" || !isValidIsoTimestamp(timestamp)) {
    return { ok: false, message: "InputEvent.timestamp must be an ISO timestamp." };
  }
  if (typeof userId !== "string" || userId.trim() === "") {
    return { ok: false, message: "InputEvent.user_id must be a non-empty string." };
  }
  if (!isRecord(payload)) {
    return { ok: false, message: "InputEvent.payload must be an object." };
  }
  if (!isRecord(context) || typeof context.timezone !== "string" || context.timezone.trim() === "") {
    return { ok: false, message: "InputEvent.context.timezone must be a non-empty string." };
  }

  return {
    ok: true,
    value: {
      event_id: eventId,
      type,
      timestamp,
      user_id: userId,
      payload,
      context: { ...context, timezone: context.timezone }
    }
  };
}

export function validateStructuredAssistantIntent(value: unknown): StructuredIntentValidationResult {
  if (!isRecord(value)) {
    return {
      ok: false,
      reason: "malformed_structured_intent",
      message: "Structured intent must be an object."
    };
  }

  if (typeof value.type !== "string" || value.type.trim() === "") {
    return {
      ok: false,
      reason: "malformed_structured_intent",
      message: "Structured intent type must be a non-empty string."
    };
  }
  if (value.type !== "calendar.create") {
    if (value.type === "meeting.notify_late") return validateMeetingNotifyLateIntent(value);
    return {
      ok: false,
      reason: "unsupported_intent_type",
      message: "Only calendar.create and meeting.notify_late structured intents are supported."
    };
  }
  if (typeof value.version !== "string" || value.version.trim() === "") {
    return {
      ok: false,
      reason: "missing_required_structured_fields",
      message: "Structured calendar intent version is required."
    };
  }
  if (value.version !== "1") {
    return {
      ok: false,
      reason: "unsupported_intent_version",
      message: "Structured calendar intent version is not supported."
    };
  }

  if (typeof value.title !== "string" || value.title.trim() === "") {
    return {
      ok: false,
      reason: "missing_required_structured_fields",
      message: "Structured calendar intent title is required."
    };
  }
  if (typeof value.start !== "string" || !isValidIsoTimestamp(value.start)) {
    return {
      ok: false,
      reason: "missing_required_structured_fields",
      message: "Structured calendar intent start must be an ISO timestamp."
    };
  }
  if (typeof value.end !== "string" || !isValidIsoTimestamp(value.end)) {
    return {
      ok: false,
      reason: "missing_required_structured_fields",
      message: "Structured calendar intent end must be an ISO timestamp."
    };
  }
  if (Date.parse(value.end) <= Date.parse(value.start)) {
    return {
      ok: false,
      reason: "invalid_time_range",
      message: "Structured calendar intent end must be after start."
    };
  }
  if (!Array.isArray(value.attendees) || value.attendees.length === 0) {
    return {
      ok: false,
      reason: "missing_required_structured_fields",
      message: "Structured calendar intent requires at least one attendee."
    };
  }

  const attendees: StructuredAttendeeReference[] = [];
  for (const attendee of value.attendees) {
    if (!isRecord(attendee)) {
      return {
        ok: false,
        reason: "invalid_attendee_reference",
        message: "Structured attendee references must be objects."
      };
    }
    const name = attendee.name;
    const id = attendee.id;
    if (typeof id === "string" && id.trim() !== "") {
      const normalizedId = id.trim();
      if (!hasValidLarkAttendeeIdPrefix(normalizedId)) {
        return {
          ok: false,
          reason: "invalid_attendee_reference",
          message: "Structured attendee id must use a supported Lark attendee prefix."
        };
      }
      attendees.push({ id: normalizedId });
      continue;
    }
    if (typeof name === "string" && name.trim() !== "") {
      attendees.push({ name: name.trim() });
      continue;
    }
    return {
      ok: false,
      reason: "invalid_attendee_reference",
      message: "Structured attendee must include a non-empty name or valid Lark attendee id."
    };
  }

  return {
    ok: true,
    value: {
      type: "calendar.create",
      version: "1",
      title: value.title.trim(),
      start: value.start,
      end: value.end,
      attendees
    }
  };
}

function validateMeetingNotifyLateIntent(value: Record<string, unknown>): StructuredIntentValidationResult {
  if (typeof value.version !== "string" || value.version.trim() === "") {
    return {
      ok: false,
      reason: "missing_required_structured_fields",
      message: "Structured meeting notification intent version is required."
    };
  }
  if (value.version !== "1") {
    return {
      ok: false,
      reason: "unsupported_intent_version",
      message: "Structured meeting notification intent version is not supported."
    };
  }

  const intent: StructuredMeetingNotifyLateIntent = {
    type: "meeting.notify_late",
    version: "1"
  };

  if (value.delay_minutes !== undefined) {
    if (
      typeof value.delay_minutes !== "number" ||
      !Number.isInteger(value.delay_minutes) ||
      value.delay_minutes < 1 ||
      value.delay_minutes > 180
    ) {
      return {
        ok: false,
        reason: "missing_required_structured_fields",
        message: "Structured meeting notification delay_minutes must be an integer from 1 to 180."
      };
    }
    intent.delay_minutes = value.delay_minutes;
  }

  if (value.message !== undefined) {
    if (typeof value.message !== "string" || value.message.trim() === "") {
      return {
        ok: false,
        reason: "missing_required_structured_fields",
        message: "Structured meeting notification message must be a non-empty string when provided."
      };
    }
    intent.message = value.message.trim().slice(0, 200);
  }

  return {
    ok: true,
    value: intent
  };
}

export function validateStructuredResponse(value: unknown): ValidationResult<StructuredResponse> {
  if (!isRecord(value)) return { ok: false, message: "StructuredResponse must be an object." };
  if (typeof value.speech !== "string") return { ok: false, message: "speech must be a string." };
  if (!isRecord(value.presentation)) return { ok: false, message: "presentation must be an object." };
  if (!Array.isArray(value.actions)) return { ok: false, message: "actions must be an array." };
  if (!isRecord(value.follow_up) || typeof value.follow_up.expected !== "boolean") {
    return { ok: false, message: "follow_up.expected must be a boolean." };
  }
  if (!isRecord(value.context_patch)) return { ok: false, message: "context_patch must be an object." };
  return { ok: true, value: value as StructuredResponse };
}

export function followUpResponse(question: string, reason: string, speech = question): StructuredResponse {
  return {
    speech,
    presentation: {
      emotion: "thinking",
      motion: "look_at_user",
      light: "soft_blink"
    },
    actions: [],
    follow_up: {
      expected: true,
      question,
      reason
    },
    context_patch: {}
  };
}

export function unsupportedEventResponse(type: string): StructuredResponse {
  return {
    speech: `暂时还不支持处理 ${type} 类型的事件。`,
    presentation: {
      emotion: "neutral",
      motion: "none",
      light: "none"
    },
    actions: [],
    follow_up: {
      expected: false
    },
    context_patch: {}
  };
}
