import type { InputEvent, StructuredResponse } from "../contracts.js";
import type { LarkIMAdapter } from "../lark/adapters.js";
export type MeetingReminderAssistantOptions = {
    imAdapter: LarkIMAdapter;
};
export declare class MeetingReminderAssistant {
    private readonly imAdapter;
    constructor(options: MeetingReminderAssistantOptions);
    handleReminder(event: InputEvent): Promise<StructuredResponse>;
    handleLateNotification(event: InputEvent): Promise<StructuredResponse>;
}
export declare function shouldRouteToMeetingNotification(event: InputEvent): boolean;
