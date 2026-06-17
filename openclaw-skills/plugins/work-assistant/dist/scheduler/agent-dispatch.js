import { spawn } from "node:child_process";
export const STACKCHAN_EVENT_SCHEMA = "openclaw.stackchan.event.v1";
export const WORK_ASSISTANT_SCHEDULER_RESPONSE_SCHEMA = "openclaw.work_assistant.scheduler_response.v1";
const DEFAULT_XIAOPAI_BASE_URL = "http://127.0.0.1:8091";
const DEFAULT_XIAOPAI_DEVICE_LOOKUP_TIMEOUT_MS = 2_000;
export function buildSchedulerAgentTurnMessage(options) {
    const { event, response, config } = options;
    return JSON.stringify({
        schema: STACKCHAN_EVENT_SCHEMA,
        event_id: event.event_id,
        ...(config.deviceId ? { device_id: config.deviceId } : {}),
        event: {
            event_id: event.event_id,
            type: "work_assistant_proactive_response",
            timestamp: event.timestamp,
            user_id: event.user_id,
            payload: {
                schema: WORK_ASSISTANT_SCHEDULER_RESPONSE_SCHEMA,
                source: "work_assistant_scheduler",
                text: response.speech,
                source_event: event,
                structured_response: response,
                agent_directive: "Use payload.structured_response as the canonical proactive reminder. Do not call workAssistant.handleEvent again for this event. Render the user-facing speech through xiaopaiControl.execute when available."
            },
            context: event.context
        },
        render: {
            target: "xiaopai",
            interrupt: config.interrupt
        }
    });
}
export async function dispatchSchedulerResponseToAgent(options) {
    const { api, event, response, config } = options;
    if (!config.enabled)
        return { status: "skipped", reason: "disabled" };
    if (response.speech.trim() === "")
        return { status: "skipped", reason: "missing_speech" };
    const sessionResolution = await resolveSchedulerAgentSession({ api, config });
    if (!sessionResolution.ok)
        return { status: "failed", code: sessionResolution.code, message: sessionResolution.message };
    const effectiveConfig = sessionResolution.deviceId && !config.deviceId
        ? { ...config, deviceId: sessionResolution.deviceId }
        : config;
    const message = buildSchedulerAgentTurnMessage({ event, response, config: effectiveConfig });
    const scheduleParams = {
        sessionKey: sessionResolution.sessionKey,
        message,
        delayMs: 0,
        deleteAfterRun: true,
        deliveryMode: config.deliveryMode,
        tag: "work-assistant-scheduler",
        name: schedulerTurnName(event.event_id),
        ...(config.agentId ? { agentId: config.agentId } : {})
    };
    const scheduleSessionTurn = api.session?.workflow?.scheduleSessionTurn ?? api.scheduleSessionTurn;
    const handle = scheduleSessionTurn ? await tryScheduleSessionTurn(scheduleSessionTurn, scheduleParams) : undefined;
    if (handle?.id) {
        api.logger?.info?.(`work-assistant scheduler queued agent turn ${JSON.stringify({
            event_id: event.event_id,
            sessionKey: handle.sessionKey,
            jobId: handle.id,
            scheduler: "session_workflow",
            sessionKeySource: sessionResolution.source,
            ...(sessionResolution.deviceId ? { deviceId: sessionResolution.deviceId } : {})
        })}`);
        return {
            status: "success",
            jobId: handle.id,
            sessionKey: handle.sessionKey
        };
    }
    const cliScheduler = api.runAgentTurnCli ?? scheduleAgentTurnWithOpenClawCron;
    try {
        const cliHandle = await cliScheduler(scheduleParams);
        if (!cliHandle?.id) {
            return {
                status: "failed",
                code: "AGENT_TURN_NOT_SCHEDULED",
                message: "OpenClaw did not accept the scheduler-produced agent turn."
            };
        }
        api.logger?.info?.(`work-assistant scheduler queued agent turn ${JSON.stringify({
            event_id: event.event_id,
            sessionKey: cliHandle.sessionKey,
            jobId: cliHandle.id,
            scheduler: "openclaw_cron_cli",
            sessionKeySource: sessionResolution.source,
            ...(sessionResolution.deviceId ? { deviceId: sessionResolution.deviceId } : {})
        })}`);
        return {
            status: "success",
            jobId: cliHandle.id,
            sessionKey: cliHandle.sessionKey
        };
    }
    catch (error) {
        const messageText = error instanceof Error ? error.message : String(error);
        return {
            status: "failed",
            code: "AGENT_TURN_SCHEDULE_FAILED",
            message: messageText
        };
    }
}
async function resolveSchedulerAgentSession(options) {
    const { api, config } = options;
    if ((config.sessionKeyMode ?? "static") !== "online_xiaopai") {
        if (!config.sessionKey) {
            return {
                ok: false,
                code: "AGENT_SESSION_KEY_MISSING",
                message: "Scheduler agent dispatch is enabled but scheduler.agentDispatch.sessionKey is missing."
            };
        }
        return { ok: true, sessionKey: config.sessionKey, source: "static" };
    }
    let deviceId;
    try {
        deviceId = api.resolveOnlineXiaopaiDeviceId
            ? await api.resolveOnlineXiaopaiDeviceId(config)
            : await resolveOnlineXiaopaiDeviceIdFromStackChan(config);
    }
    catch (error) {
        const message = error instanceof Error ? error.message : String(error);
        return {
            ok: false,
            code: "AGENT_SESSION_KEY_DEVICE_LOOKUP_FAILED",
            message: `Unable to resolve an online Xiaopai device for scheduler.agentDispatch.sessionKeyMode=online_xiaopai: ${message}`
        };
    }
    if (!deviceId) {
        return {
            ok: false,
            code: "AGENT_SESSION_KEY_DEVICE_MISSING",
            message: "Unable to resolve an online Xiaopai device for scheduler.agentDispatch.sessionKeyMode=online_xiaopai."
        };
    }
    return {
        ok: true,
        sessionKey: buildDynamicXiaopaiSessionKey(config, deviceId),
        source: "online_xiaopai",
        deviceId
    };
}
async function resolveOnlineXiaopaiDeviceIdFromStackChan(config) {
    const baseUrl = (config.xiaopaiBaseUrl ?? process.env.STACKCHAN_BASE_URL ?? DEFAULT_XIAOPAI_BASE_URL).replace(/\/+$/, "");
    const timeoutMs = config.xiaopaiDeviceLookupTimeoutMs ?? DEFAULT_XIAOPAI_DEVICE_LOOKUP_TIMEOUT_MS;
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    try {
        const response = await fetch(`${baseUrl}/devices`, { signal: controller.signal });
        const text = await response.text();
        if (!response.ok)
            throw new Error(`stack-chan /devices returned HTTP ${response.status}`);
        const parsed = parseJson(text);
        if (parsed === undefined)
            throw new Error("stack-chan /devices returned malformed JSON");
        return selectOnlineXiaopaiDeviceId(parsed);
    }
    finally {
        clearTimeout(timer);
    }
}
export function buildDynamicXiaopaiSessionKey(config, deviceId) {
    const safeDeviceId = safeOpenClawSessionPart(deviceId);
    if (config.sessionKey?.includes("{device_id}")) {
        return config.sessionKey.replaceAll("{device_id}", safeDeviceId);
    }
    const marker = "xiaopai-";
    const markerIndex = config.sessionKey?.lastIndexOf(marker) ?? -1;
    if (config.sessionKey && markerIndex >= 0) {
        return `${config.sessionKey.slice(0, markerIndex + marker.length)}${safeDeviceId}`;
    }
    const agentId = safeOpenClawSessionPart(config.agentId ?? "main");
    return `agent:${agentId}:xiaopai-${safeDeviceId}`;
}
export function selectOnlineXiaopaiDeviceId(value) {
    if (!isRecord(value))
        return undefined;
    const realtimeDeviceId = selectDeviceId(value.realtime_devices, true);
    if (realtimeDeviceId)
        return realtimeDeviceId;
    const onlineDeviceId = selectDeviceId(value.devices, true);
    if (onlineDeviceId)
        return onlineDeviceId;
    const defaultDeviceId = readDeviceId(value.default_device_id);
    return defaultDeviceId && defaultDeviceId !== "default" ? defaultDeviceId : undefined;
}
function selectDeviceId(value, requireOnline) {
    if (!Array.isArray(value))
        return undefined;
    for (const item of value) {
        if (!isRecord(item))
            continue;
        if (requireOnline && item.online !== true)
            continue;
        const deviceId = readDeviceId(item.device_id);
        if (deviceId)
            return deviceId;
    }
    return undefined;
}
function readDeviceId(value) {
    return typeof value === "string" && value.trim() !== "" ? value.trim() : undefined;
}
function safeOpenClawSessionPart(value) {
    const safe = value.trim().replace(/[^A-Za-z0-9_.:-]+/g, "_").slice(0, 64);
    return safe || "default";
}
function parseJson(text) {
    try {
        return JSON.parse(text);
    }
    catch {
        return undefined;
    }
}
async function tryScheduleSessionTurn(scheduleSessionTurn, params) {
    try {
        return await scheduleSessionTurn(params);
    }
    catch {
        return undefined;
    }
}
export async function scheduleAgentTurnWithOpenClawCron(params) {
    const args = buildOpenClawCronAddArgs(params);
    const result = await runOpenClawCli(args);
    const parsed = JSON.parse(result.stdout);
    const job = readCronJobPayload(parsed);
    if (!job.id)
        return undefined;
    return {
        id: job.id,
        pluginId: "work-assistant",
        sessionKey: job.sessionKey ?? params.sessionKey,
        kind: "cron-agent-turn"
    };
}
export function buildOpenClawCronAddArgs(params) {
    const args = [
        "cron",
        "add",
        "--json",
        "--at",
        "1s",
        "--wake",
        "now",
        "--delete-after-run",
        "--session-key",
        params.sessionKey,
        "--session",
        "isolated",
        "--message",
        params.message,
        "--name",
        params.name,
        "--description",
        params.tag,
        "--light-context",
        "--thinking",
        "low",
        "--no-deliver"
    ];
    if (params.agentId)
        args.push("--agent", params.agentId);
    if (params.deliveryMode === "announce")
        args.push("--announce");
    return args;
}
function runOpenClawCli(args) {
    return new Promise((resolve, reject) => {
        const child = spawn(process.env.OPENCLAW_CLI_PATH ?? "openclaw", args, {
            stdio: ["ignore", "pipe", "pipe"],
            env: process.env
        });
        const stdout = [];
        const stderr = [];
        child.stdout.on("data", (chunk) => stdout.push(Buffer.from(chunk)));
        child.stderr.on("data", (chunk) => stderr.push(Buffer.from(chunk)));
        child.on("error", reject);
        child.on("close", (code) => {
            const out = Buffer.concat(stdout).toString("utf8");
            const err = Buffer.concat(stderr).toString("utf8");
            if (code === 0) {
                resolve({ stdout: out, stderr: err });
            }
            else {
                reject(new Error(err.trim() || `openclaw exited with code ${code ?? "unknown"}`));
            }
        });
    });
}
function readCronJobPayload(value) {
    if (!isRecord(value))
        return {};
    const job = isRecord(value.job) ? value.job : value;
    return {
        ...(typeof job.id === "string" ? { id: job.id } : {}),
        ...(typeof job.sessionKey === "string" ? { sessionKey: job.sessionKey } : {})
    };
}
function schedulerTurnName(eventId) {
    const normalized = eventId.replace(/[^A-Za-z0-9_.-]+/g, "_").slice(0, 80);
    return normalized ? `scheduler_${normalized}` : "scheduler_event";
}
function isRecord(value) {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}
//# sourceMappingURL=agent-dispatch.js.map