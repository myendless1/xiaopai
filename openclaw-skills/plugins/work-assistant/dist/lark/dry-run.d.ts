import type { LarkCalendarAdapter, LarkCalendarCreateRequest, LarkCalendarCreateResult, LarkCalendarListRequest, LarkCalendarListResult, LarkContactAdapter, LarkIMAdapter, LarkMessageSendRequest, LarkMessageSendResult, LarkPersonResolution } from "./adapters.js";
export declare class DryRunContactAdapter implements LarkContactAdapter {
    resolvePeople(names: string[]): Promise<Record<string, LarkPersonResolution>>;
}
export declare class DryRunCalendarAdapter implements LarkCalendarAdapter {
    createEvent(request: LarkCalendarCreateRequest): Promise<LarkCalendarCreateResult>;
    listEvents(request: LarkCalendarListRequest): Promise<LarkCalendarListResult>;
}
export declare class DryRunIMAdapter implements LarkIMAdapter {
    readonly sentMessages: LarkMessageSendRequest[];
    sendText(request: LarkMessageSendRequest): Promise<LarkMessageSendResult>;
}
