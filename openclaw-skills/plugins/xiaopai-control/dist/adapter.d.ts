import type { XiaopaiCommand, XiaopaiCommandResult, XiaopaiDeviceListResult, XiaopaiHealthResult } from "./contracts.js";
export type XiaopaiControlAdapter = {
    execute(command: XiaopaiCommand): Promise<XiaopaiCommandResult>;
    getHealth(): Promise<XiaopaiHealthResult>;
    listDevices(): Promise<XiaopaiDeviceListResult>;
};
export type XiaopaiHttpAdapterOptions = {
    baseUrl: string;
    timeoutMs: number;
    fetch?: typeof fetch;
};
