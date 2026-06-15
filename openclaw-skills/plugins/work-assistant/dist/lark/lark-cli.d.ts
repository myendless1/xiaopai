import type { LarkCalendarAdapter, LarkCalendarCreateRequest, LarkCalendarCreateResult, LarkCalendarListRequest, LarkCalendarListResult, LarkContactAdapter, LarkIMAdapter, LarkMessageSendRequest, LarkMessageSendResult, NormalizedAgendaEvent, LarkPersonResolution } from "./adapters.js";
export type ProcessResult = {
    code: number;
    stdout: string;
    stderr: string;
};
export type ProcessRunner = (argv: string[], options: {
    timeoutMs: number;
}) => Promise<ProcessResult>;
export type LarkCliAdapterOptions = {
    cliPath?: string;
    timeoutMs?: number;
    identity?: "user" | "bot";
    runner?: ProcessRunner;
};
export declare function createNodeProcessRunner(cliPath?: string): ProcessRunner;
export declare function normalizeAgendaEvents(payload: unknown): NormalizedAgendaEvent[];
export declare class LarkCliContactAdapter implements LarkContactAdapter {
    private readonly runner;
    private readonly timeoutMs;
    private readonly identity;
    constructor(options?: LarkCliAdapterOptions);
    resolvePeople(names: string[]): Promise<Record<string, LarkPersonResolution>>;
}
export declare class LarkCliCalendarAdapter implements LarkCalendarAdapter {
    private readonly runner;
    private readonly timeoutMs;
    private readonly identity;
    constructor(options?: LarkCliAdapterOptions);
    createEvent(request: LarkCalendarCreateRequest): Promise<LarkCalendarCreateResult>;
    listEvents(request: LarkCalendarListRequest): Promise<LarkCalendarListResult>;
}
export declare class LarkCliIMAdapter implements LarkIMAdapter {
    private readonly runner;
    private readonly timeoutMs;
    private readonly identity;
    constructor(options?: LarkCliAdapterOptions);
    sendText(request: LarkMessageSendRequest): Promise<LarkMessageSendResult>;
}
