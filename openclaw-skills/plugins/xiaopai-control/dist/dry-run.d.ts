import type { XiaopaiControlAdapter } from "./adapter.js";
import type { XiaopaiCommand, XiaopaiCommandResult, XiaopaiDeviceListResult, XiaopaiHealthResult } from "./contracts.js";
export declare class XiaopaiDryRunAdapter implements XiaopaiControlAdapter {
    execute(command: XiaopaiCommand): Promise<XiaopaiCommandResult>;
    getHealth(): Promise<XiaopaiHealthResult>;
    listDevices(): Promise<XiaopaiDeviceListResult>;
}
