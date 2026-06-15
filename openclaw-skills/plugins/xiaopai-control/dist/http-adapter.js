import { failedCommandResult, queuedCommandResult } from "./results.js";
export class XiaopaiHttpAdapter {
    baseUrl;
    timeoutMs;
    fetchFn;
    constructor(options) {
        this.baseUrl = options.baseUrl.replace(/\/+$/, "");
        this.timeoutMs = options.timeoutMs;
        this.fetchFn = options.fetch ?? fetch;
    }
    async execute(command) {
        if (command.type === "stop")
            return this.stop(command);
        return this.postCommand(command);
    }
    async getHealth() {
        const response = await this.requestJson("GET", "/health");
        if (!response.ok) {
            return {
                status: "failed",
                reachable: false,
                error: { code: response.code, message: response.message }
            };
        }
        const body = response.body;
        if (!isRecord(body)) {
            return {
                status: "failed",
                reachable: false,
                error: { code: "malformed_response", message: "Health response was not a JSON object." }
            };
        }
        return {
            status: "ok",
            reachable: body.ok === true,
            ...(typeof body.service === "string" ? { service: body.service } : {}),
            details: sanitizeDetails(body)
        };
    }
    async listDevices() {
        const response = await this.requestJson("GET", "/devices");
        if (!response.ok) {
            return {
                status: "failed",
                devices: [],
                error: { code: response.code, message: response.message }
            };
        }
        const body = response.body;
        if (!isRecord(body) || !Array.isArray(body.devices)) {
            return {
                status: "failed",
                devices: [],
                error: { code: "malformed_response", message: "Device list response was not valid." }
            };
        }
        return {
            status: "ok",
            ...(typeof body.default_device_id === "string" ? { default_device_id: body.default_device_id } : {}),
            ...(typeof body.online_ttl_seconds === "number" ? { online_ttl_seconds: body.online_ttl_seconds } : {}),
            devices: body.devices.flatMap(normalizeDevice)
        };
    }
    async postCommand(command) {
        const response = await this.requestJson("POST", "/command", toStackChanPostBody(command));
        if (!response.ok) {
            return failedCommandResult(command.type, response.code, response.message, response.details, "device_id" in command ? command.device_id : undefined);
        }
        return normalizeQueueResponse(command, response.body);
    }
    async stop(command) {
        const query = command.device_id ? `?device_id=${encodeURIComponent(command.device_id)}` : "";
        const response = await this.requestJson("GET", `/command/stop${query}`);
        if (!response.ok) {
            return failedCommandResult("stop", response.code, response.message, response.details, command.device_id);
        }
        return normalizeQueueResponse(command, response.body);
    }
    async requestJson(method, path, body) {
        const controller = new AbortController();
        const timer = setTimeout(() => controller.abort(), this.timeoutMs);
        try {
            const response = await this.fetchFn(`${this.baseUrl}${path}`, {
                method,
                signal: controller.signal,
                ...(body === undefined
                    ? {}
                    : {
                        headers: { "content-type": "application/json" },
                        body: JSON.stringify(body)
                    })
            });
            const text = await response.text();
            const parsed = parseJson(text);
            if (!response.ok) {
                return {
                    ok: false,
                    code: "http_error",
                    message: `stack-chan server returned HTTP ${response.status}.`,
                    details: { status: response.status, body: parsed ?? truncate(text) }
                };
            }
            if (parsed === undefined) {
                return { ok: false, code: "malformed_response", message: "stack-chan response was not valid JSON." };
            }
            return { ok: true, body: parsed };
        }
        catch (error) {
            return normalizeFetchError(error);
        }
        finally {
            clearTimeout(timer);
        }
    }
}
function toStackChanPostBody(command) {
    const base = {
        type: command.type,
        ...("device_id" in command && command.device_id ? { device_id: command.device_id } : {}),
        ...("interrupt" in command && command.interrupt !== undefined ? { interrupt: command.interrupt } : {})
    };
    switch (command.type) {
        case "speak":
            return { ...base, payload: { text: command.text } };
        case "face":
            return { ...base, payload: { expression: command.expression } };
        case "action":
            return { ...base, payload: { expression: command.action } };
        case "move":
            return {
                ...base,
                payload: {
                    type: command.direction,
                    ...(command.degree === undefined ? {} : { degree: command.degree }),
                    ...(command.duration_ms === undefined ? {} : { duration_ms: command.duration_ms })
                }
            };
        case "sequence":
            return { ...base, payload: command.steps.map(toStackChanSequenceStep) };
    }
}
function toStackChanSequenceStep(step) {
    switch (step.type) {
        case "speak":
            return { type: "speak", text: step.text };
        case "face":
            return { type: "face", expression: step.expression };
        case "action":
            return { type: "face", expression: step.action };
        case "move":
            return {
                type: "move",
                action: step.direction,
                ...(step.degree === undefined ? {} : { degree: step.degree }),
                ...(step.duration_ms === undefined ? {} : { duration_ms: step.duration_ms })
            };
    }
}
function normalizeQueueResponse(command, body) {
    if (!isRecord(body) || body.type !== "queued" || !isRecord(body.command)) {
        return failedCommandResult(command.type, "malformed_response", "stack-chan queue response was not valid.", undefined, "device_id" in command ? command.device_id : undefined);
    }
    const cmdId = typeof body.command.cmd_id === "string" ? body.command.cmd_id : undefined;
    return queuedCommandResult(command, {
        ...(cmdId ? { cmd_id: cmdId } : {}),
        stack_command_type: typeof body.command.type === "string" ? body.command.type : command.type,
        interrupt: body.command.interrupt === true
    }, typeof body.device_id === "string" ? body.device_id : "device_id" in command ? command.device_id : undefined);
}
function normalizeDevice(value) {
    if (!isRecord(value) || typeof value.device_id !== "string")
        return [];
    return [
        {
            device_id: value.device_id,
            ...(typeof value.online === "boolean" ? { online: value.online } : {}),
            ...(typeof value.pending_commands === "number" ? { pending_commands: value.pending_commands } : {}),
            ...(typeof value.last_seen_seconds_ago === "number" ? { last_seen_seconds_ago: value.last_seen_seconds_ago } : {}),
            ...("last_ack" in value ? { last_ack: value.last_ack } : {})
        }
    ];
}
function normalizeFetchError(error) {
    if (isRecord(error) && error.name === "AbortError") {
        return { ok: false, code: "timeout", message: "stack-chan request timed out." };
    }
    return {
        ok: false,
        code: "network_error",
        message: error instanceof Error ? sanitizeErrorMessage(error.message) : "stack-chan request failed."
    };
}
function parseJson(text) {
    if (text.trim() === "")
        return undefined;
    try {
        return JSON.parse(text);
    }
    catch {
        return undefined;
    }
}
function sanitizeDetails(value) {
    const { openclaw, head_touch_events: _headTouchEvents, ...rest } = value;
    return { ...rest, openclaw_enabled: isRecord(openclaw) ? openclaw.enabled === true : undefined };
}
function sanitizeErrorMessage(message) {
    return message.replace(/https?:\/\/[^\s)]+/g, "[url]").slice(0, 300);
}
function truncate(value) {
    return value.length > 300 ? `${value.slice(0, 300)}...` : value;
}
function isRecord(value) {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}
//# sourceMappingURL=http-adapter.js.map