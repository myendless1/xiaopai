import type { LarkIMAdapter, LarkMessageSendRequest, LarkMessageSendResult } from "./adapters.js";
import { type ProcessRunner } from "./lark-cli.js";
export type OpenClawFeishuIMAdapterOptions = {
    cliPath?: string;
    timeoutMs?: number;
    channel?: string;
    accountId?: string;
    runner?: ProcessRunner;
};
export declare class OpenClawFeishuIMAdapter implements LarkIMAdapter {
    private readonly runner;
    private readonly timeoutMs;
    private readonly channel;
    private readonly accountId;
    constructor(options?: OpenClawFeishuIMAdapterOptions);
    sendText(request: LarkMessageSendRequest): Promise<LarkMessageSendResult>;
}
