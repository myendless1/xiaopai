import type { InputEvent, MeetingNotificationTarget, StructuredResponse } from "../contracts.js";
import type { LarkIMAdapter } from "../lark/adapters.js";
export type MeetingReminderAssistantOptions = {
    imAdapter: LarkIMAdapter;
    defaultNotificationTarget?: MeetingNotificationTarget;
};
export declare class MeetingReminderAssistant {
    private readonly imAdapter;
    private readonly defaultNotificationTarget;
    constructor(options: MeetingReminderAssistantOptions);
    handleReminder(event: InputEvent): Promise<StructuredResponse>;
    handleLateNotification(event: InputEvent): Promise<StructuredResponse>;
}
export declare function shouldRouteToMeetingNotification(event: InputEvent): boolean;
