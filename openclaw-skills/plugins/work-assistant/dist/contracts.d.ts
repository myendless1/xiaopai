export type SupportedAssistantEventType = "user_utterance" | "head_touch" | "daily_briefing_triggered" | "meeting_starting_soon" | "outdoor_event_detected" | "business_trip_tomorrow_detected" | "sedentary_detected" | "wellbeing_companion_requested";
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
export type StructuredAttendeeReference = {
    name: string;
    id?: never;
} | {
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
export type StructuredIntentValidationReason = "malformed_structured_intent" | "unsupported_intent_type" | "unsupported_intent_version" | "missing_required_structured_fields" | "invalid_time_range" | "invalid_attendee_reference";
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
export type ValidationResult<T> = {
    ok: true;
    value: T;
} | {
    ok: false;
    message: string;
};
export type StructuredIntentValidationResult = {
    ok: true;
    value: StructuredAssistantIntent;
} | {
    ok: false;
    reason: StructuredIntentValidationReason;
    message: string;
};
export declare function validateInputEvent(value: unknown): ValidationResult<InputEvent>;
export declare function validateStructuredAssistantIntent(value: unknown): StructuredIntentValidationResult;
export declare function validateStructuredResponse(value: unknown): ValidationResult<StructuredResponse>;
export declare function followUpResponse(question: string, reason: string, speech?: string): StructuredResponse;
export declare function unsupportedEventResponse(type: string): StructuredResponse;
