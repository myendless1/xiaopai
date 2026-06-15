import type { XiaopaiControlAdapter, XiaopaiHttpAdapterOptions } from "./adapter.js";
import type { XiaopaiCommand, XiaopaiCommandResult, XiaopaiDeviceListResult, XiaopaiHealthResult } from "./contracts.js";
export declare class XiaopaiHttpAdapter implements XiaopaiControlAdapter {
    private readonly baseUrl;
    private readonly timeoutMs;
    private readonly fetchFn;
    constructor(options: XiaopaiHttpAdapterOptions);
    execute(command: XiaopaiCommand): Promise<XiaopaiCommandResult>;
    getHealth(): Promise<XiaopaiHealthResult>;
    listDevices(): Promise<XiaopaiDeviceListResult>;
    private postCommand;
    private stop;
    private requestJson;
}
