import type { XiaopaiControlAdapter } from "./adapter.js";
import type { XiaopaiCommand, XiaopaiCommandResult, XiaopaiDeviceListResult, XiaopaiHealthResult } from "./contracts.js";
export type XiaopaiControlHandlerOptions = {
    adapter: XiaopaiControlAdapter;
    defaultDeviceId?: string;
};
export type XiaopaiControlHandler = {
    execute(input: unknown): Promise<XiaopaiCommandResult>;
    getHealth(): Promise<XiaopaiHealthResult>;
    listDevices(): Promise<XiaopaiDeviceListResult>;
};
export declare function createXiaopaiControlHandler(options: XiaopaiControlHandlerOptions): XiaopaiControlHandler;
export declare function applyDefaultDeviceId(command: XiaopaiCommand, defaultDeviceId: string | undefined): XiaopaiCommand;
