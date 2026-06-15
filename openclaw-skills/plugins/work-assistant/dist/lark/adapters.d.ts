export type LarkPerson = {
    id: string;
    name: string;
    email?: string;
    department?: string;
};
export type LarkPersonResolution = {
    status: "unique";
    person: LarkPerson;
} | {
    status: "ambiguous";
    candidates: LarkPerson[];
} | {
    status: "missing";
};
export interface LarkContactAdapter {
    resolvePeople(names: string[]): Promise<Record<string, LarkPersonResolution>>;
}
export type LarkCalendarCreateRequest = {
    title: string;
    start: string;
    end: string;
    requesterId: string;
    attendeeIds: string[];
    description?: string;
};
export type LarkCalendarCreateResult = {
    ok: true;
    eventId: string;
    calendarId?: string;
    link?: string;
} | {
    ok: false;
    code: string;
    message: string;
};
export type AgendaEventCategory = "internal_meeting" | "customer_reception" | "outdoor_activity" | "deep_work" | "uncategorized";
export type NormalizedAgendaEvent = {
    id: string;
    title: string;
    start: string;
    end: string;
    calendarId?: string;
    location?: string;
    description?: string;
    attendeeCount?: number;
    notificationTarget?: LarkMessageNotificationTarget;
};
export type LarkCalendarListRequest = {
    start: string;
    end: string;
    calendarId?: string;
};
export type LarkCalendarListResult = {
    ok: true;
    events: NormalizedAgendaEvent[];
    calendarId?: string;
} | {
    ok: false;
    code: string;
    message: string;
};
export interface LarkCalendarAdapter {
    createEvent(request: LarkCalendarCreateRequest): Promise<LarkCalendarCreateResult>;
    listEvents(request: LarkCalendarListRequest): Promise<LarkCalendarListResult>;
}
export type LarkMessageNotificationTarget = {
    chatId?: string;
    attendeeUserIds?: string[];
};
export type LarkMessageSendRequest = {
    text: string;
    requesterId: string;
    chatId?: string;
    attendeeUserIds?: string[];
    idempotencyKey?: string;
};
export type LarkMessageSendResult = {
    ok: true;
    messageId: string;
    chatId?: string;
    attendeeUserIds?: string[];
} | {
    ok: false;
    code: string;
    message: string;
};
export interface LarkIMAdapter {
    sendText(request: LarkMessageSendRequest): Promise<LarkMessageSendResult>;
}
