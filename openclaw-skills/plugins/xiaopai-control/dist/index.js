import { definePluginEntry } from "openclaw/plugin-sdk/plugin-entry";
import { DEFAULT_BASE_URL, DEFAULT_TIMEOUT_MS } from "./constants.js";
import { XiaopaiDryRunAdapter } from "./dry-run.js";
import { createXiaopaiControlHandler } from "./handler.js";
import { XiaopaiHttpAdapter } from "./http-adapter.js";
import { detectXiaopaiRenderIntentFromMessages, detectXiaopaiRenderIntentFromPrompt, detectXiaopaiRenderIntentFromSessionKey, diagnosticBase, executeXiaopaiRenderFallback, isXiaopaiExecuteTool, observeXiaopaiExecuteCall, observeXiaopaiExecuteCliCall } from "./render-fallback.js";
export function readPluginConfig(api) {
    const raw = typeof api.pluginConfig === "object" && api.pluginConfig !== null ? api.pluginConfig : {};
    const config = {
        baseUrl: typeof raw.baseUrl === "string" && raw.baseUrl.trim() !== "" ? raw.baseUrl.trim() : DEFAULT_BASE_URL,
        timeoutMs: typeof raw.timeoutMs === "number" && Number.isFinite(raw.timeoutMs) && raw.timeoutMs > 0 ? raw.timeoutMs : DEFAULT_TIMEOUT_MS,
        dryRun: raw.dryRun === true
    };
    if (typeof raw.defaultDeviceId === "string" && raw.defaultDeviceId.trim() !== "") {
        config.defaultDeviceId = raw.defaultDeviceId.trim();
    }
    return config;
}
export function createDefaultXiaopaiControlRuntime(api, adapterOverride) {
    const config = readPluginConfig(api);
    const adapter = adapterOverride ??
        (config.dryRun
            ? new XiaopaiDryRunAdapter()
            : new XiaopaiHttpAdapter({
                baseUrl: config.baseUrl,
                timeoutMs: config.timeoutMs
            }));
    return {
        config,
        adapter,
        handler: createXiaopaiControlHandler({
            adapter,
            ...(config.defaultDeviceId ? { defaultDeviceId: config.defaultDeviceId } : {})
        })
    };
}
export function registerXiaopaiRenderFallbackHooks(api, handler) {
    const states = new Map();
    const hookApi = api;
    const registerHook = (hookName, hookHandler, options) => {
        if (typeof hookApi.on === "function") {
            hookApi.on(hookName, hookHandler, options);
            return;
        }
        if (typeof api.registerHook === "function") {
            api.registerHook(hookName, hookHandler, options);
        }
    };
    registerHook("before_agent_run", (event, ctx) => {
        const key = renderStateKey(event, ctx);
        if (!key)
            return { outcome: "pass" };
        const promptDetection = detectXiaopaiRenderIntentFromPrompt(event.prompt);
        const detection = promptDetection.required || !Array.isArray(event.messages)
            ? promptDetection
            : detectXiaopaiRenderIntentFromMessages(event.messages);
        const sessionDetection = detection.required ? detection : detectXiaopaiRenderIntentFromSessionKey(readSessionKey(event, ctx));
        if (!sessionDetection.required) {
            logDiagnostic(api, { outcome: "fallback_skipped", reason: sessionDetection.reason }, "debug");
            states.delete(key);
            return { outcome: "pass" };
        }
        states.set(key, { context: sessionDetection.context, speechRendered: false });
        return { outcome: "pass" };
    }, { priority: -50 });
    registerHook("after_tool_call", (event, ctx) => {
        const observedSpeechRender = isXiaopaiExecuteTool(event.toolName)
            ? observeXiaopaiExecuteCall(event.params, event.result)
            : isExecTool(event.toolName)
                ? observeXiaopaiExecuteCliCall(event.params, event.result)
                : false;
        if (!observedSpeechRender)
            return;
        const key = renderStateKey(event, ctx);
        if (!key)
            return;
        const state = states.get(key);
        if (!state || state.speechRendered)
            return;
        state.speechRendered = true;
        state.diagnostic = {
            ...diagnosticBase(state.context),
            outcome: "explicit_rendered"
        };
        logDiagnostic(api, state.diagnostic);
    });
    registerHook("before_agent_finalize", async (event, ctx) => {
        const key = renderStateKey(event, ctx);
        if (!key)
            return;
        const state = states.get(key);
        if (!state)
            return;
        const execution = await executeXiaopaiRenderFallback({
            state,
            finalText: event.lastAssistantMessage,
            execute: (input) => handler.execute(input)
        });
        const diagnostic = "diagnostic" in execution ? execution.diagnostic : execution;
        state.diagnostic = diagnostic;
        logDiagnostic(api, diagnostic, diagnostic.outcome === "fallback_failed" ? "warn" : "info");
        return { action: "continue", reason: "xiaopai render fallback observed" };
    }, { priority: -50, timeoutMs: 20_000 });
    registerHook("agent_end", (event, ctx) => {
        const key = renderStateKey(event, ctx);
        if (key)
            states.delete(key);
    });
}
export const xiaopaiControlPlugin = definePluginEntry({
    id: "xiaopai-control",
    name: "Xiaopai Control",
    description: "Validated Xiaopai stack-chan command execution, health, and device listing.",
    register(api) {
        const runtime = createDefaultXiaopaiControlRuntime(api);
        const handler = runtime.handler;
        api.registerGatewayMethod("xiaopaiControl.execute", async ({ params, respond }) => {
            respond(true, await handler.execute(params));
        }, { scope: "operator.write" });
        api.registerGatewayMethod("tool.xiaopaiControl.execute", async ({ params, respond }) => {
            respond(true, await handler.execute(params));
        }, { scope: "operator.write" });
        api.registerGatewayMethod("xiaopaiControl.getHealth", async ({ respond }) => {
            respond(true, await handler.getHealth());
        }, { scope: "operator.read" });
        api.registerGatewayMethod("xiaopaiControl.listDevices", async ({ respond }) => {
            respond(true, await handler.listDevices());
        }, { scope: "operator.read" });
        registerXiaopaiRenderFallbackHooks(api, handler);
    }
});
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
function renderStateKey(event, ctx) {
    return readString(event.runId) ?? readString(ctx.runId) ?? readString(event.sessionKey) ?? readString(ctx.sessionKey) ?? readString(event.sessionId) ?? readString(ctx.sessionId);
}
function readSessionKey(event, ctx) {
    return readString(event.sessionKey) ?? readString(ctx.sessionKey) ?? readString(event.sessionId) ?? readString(ctx.sessionId);
}
function readString(value) {
    return typeof value === "string" && value.trim() !== "" ? value : undefined;
}
function isExecTool(toolName) {
    return typeof toolName === "string" && (toolName === "exec" || toolName.endsWith(".exec"));
}
function logDiagnostic(api, diagnostic, level = "info") {
    const message = `xiaopai render fallback ${JSON.stringify(diagnostic)}`;
    if (level === "debug" && api.logger.debug) {
        api.logger.debug(message);
    }
    else if (level === "warn") {
        api.logger.warn(message);
    }
    else {
        api.logger.info(message);
    }
}
//# sourceMappingURL=index.js.map