import { createNodeProcessRunner } from "./lark-cli.js";
const DEFAULT_TIMEOUT_MS = 15000;
export class OpenClawFeishuIMAdapter {
    runner;
    timeoutMs;
    channel;
    accountId;
    constructor(options = {}) {
        this.runner = options.runner ?? createNodeProcessRunner(options.cliPath ?? "openclaw");
        this.timeoutMs = options.timeoutMs ?? DEFAULT_TIMEOUT_MS;
        this.channel = options.channel ?? "feishu";
        this.accountId = options.accountId;
    }
    async sendText(request) {
        const target = buildOpenClawTarget(request);
        if (!target) {
            return {
                ok: false,
                code: "OPENCLAW_MESSAGE_TARGET_MISSING",
                message: "A chat id or attendee user id is required to send an OpenClaw Feishu message."
            };
        }
        const argv = [
            "--no-color",
            "message",
            "send",
            "--channel",
            this.channel,
            "--target",
            target,
            "--message",
            request.text,
            "--json"
        ];
        if (this.accountId)
            argv.push("--account", this.accountId);
        const result = await this.runner(argv, { timeoutMs: this.timeoutMs });
        if (result.code !== 0) {
            return {
                ok: false,
                code: "OPENCLAW_MESSAGE_SEND_FAILED",
                message: (result.stderr.trim() || result.stdout.trim() || "openclaw message send failed.").slice(0, 1000)
            };
        }
        try {
            const payload = parseOpenClawJson(result.stdout);
            const messageId = extractMessageId(payload);
            if (!messageId) {
                return {
                    ok: false,
                    code: "OPENCLAW_MESSAGE_PARSE_FAILED",
                    message: "OpenClaw message send response did not include a messageId."
                };
            }
            const success = {
                ok: true,
                messageId
            };
            if (request.chatId)
                success.chatId = request.chatId;
            if (request.attendeeUserIds && request.attendeeUserIds.length > 0) {
                success.attendeeUserIds = request.attendeeUserIds;
            }
            return success;
        }
        catch (error) {
            return {
                ok: false,
                code: "OPENCLAW_MESSAGE_PARSE_FAILED",
                message: error instanceof Error ? error.message : String(error)
            };
        }
    }
}
function buildOpenClawTarget(request) {
    if (request.chatId)
        return withTargetPrefix(request.chatId, "chat");
    const firstUserId = request.attendeeUserIds?.[0];
    if (firstUserId)
        return withTargetPrefix(firstUserId, "user");
    return undefined;
}
function withTargetPrefix(value, prefix) {
    const trimmed = value.trim();
    if (/^(chat|group|channel|user|dm|open_id):/i.test(trimmed))
        return trimmed;
    return `${prefix}:${trimmed}`;
}
function parseOpenClawJson(stdout) {
    const trimmed = stdout.trim();
    if (!trimmed)
        throw new Error("openclaw returned empty JSON output.");
    try {
        return JSON.parse(trimmed);
    }
    catch {
        const jsonStart = trimmed.indexOf("{");
        if (jsonStart < 0)
            throw new Error("openclaw output did not include a JSON object.");
        return JSON.parse(trimmed.slice(jsonStart));
    }
}
function extractMessageId(payload) {
    const root = asRecord(payload);
    if (!root)
        return undefined;
    const payloadRecord = asRecord(root.payload);
    const receipt = asRecord(payloadRecord?.receipt);
    return (readString(root, "messageId") ??
        readString(payloadRecord, "messageId") ??
        readString(receipt, "primaryPlatformMessageId"));
}
function asRecord(value) {
    return typeof value === "object" && value !== null && !Array.isArray(value)
        ? value
        : undefined;
}
function readString(record, key) {
    const value = record?.[key];
    return typeof value === "string" && value.trim() !== "" ? value : undefined;
}
//# sourceMappingURL=openclaw-feishu.js.map