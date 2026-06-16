import { type OpenClawPluginApi } from "openclaw/plugin-sdk/plugin-entry";
import type { XiaopaiControlAdapter } from "./adapter.js";
import type { XiaopaiControlConfig } from "./contracts.js";
import { createXiaopaiControlHandler } from "./handler.js";
export type XiaopaiControlPluginEntry = {
    id: string;
    name: string;
    description: string;
    configSchema: unknown;
    register(api: OpenClawPluginApi): void;
};
export declare function readPluginConfig(api: {
    pluginConfig?: unknown;
}): XiaopaiControlConfig;
export declare function createDefaultXiaopaiControlRuntime(api: {
    pluginConfig?: unknown;
}, adapterOverride?: XiaopaiControlAdapter): {
    config: XiaopaiControlConfig;
    adapter: XiaopaiControlAdapter;
    handler: import("./handler.js").XiaopaiControlHandler;
};
export declare function registerXiaopaiRenderFallbackHooks(api: OpenClawPluginApi, handler: ReturnType<typeof createXiaopaiControlHandler>): void;
export declare const xiaopaiControlPlugin: XiaopaiControlPluginEntry;
export default xiaopaiControlPlugin;
export { XiaopaiDryRunAdapter } from "./dry-run.js";
export { createXiaopaiControlHandler, applyDefaultDeviceId } from "./handler.js";
export { XiaopaiHttpAdapter } from "./http-adapter.js";
export * from "./adapter.js";
export * from "./constants.js";
export * from "./contracts.js";
export * from "./render-fallback.js";
export * from "./speech-text.js";
export * from "./validation.js";
