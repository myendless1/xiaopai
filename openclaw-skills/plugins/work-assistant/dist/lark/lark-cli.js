import { spawn } from "node:child_process";
const DEFAULT_TIMEOUT_MS = 15000;
export function createNodeProcessRunner(cliPath = "lark-cli") {
    return (argv, options) => new Promise((resolve) => {
        const child = spawn(cliPath, argv, {
            stdio: ["ignore", "pipe", "pipe"],
            shell: false
        });
        const stdout = [];
        const stderr = [];
        const timeout = setTimeout(() => {
            child.kill("SIGTERM");
        }, options.timeoutMs);
        child.stdout.on("data", (chunk) => stdout.push(chunk));
        child.stderr.on("data", (chunk) => stderr.push(chunk));
        child.on("close", (code) => {
            clearTimeout(timeout);
            resolve({
                code: code ?? 1,
                stdout: Buffer.concat(stdout).toString("utf8"),
                stderr: Buffer.concat(stderr).toString("utf8")
            });
        });
        child.on("error", (error) => {
            clearTimeout(timeout);
            resolve({
                code: 1,
                stdout: "",
                stderr: error.message
            });
        });
    });
}
function parseJson(stdout) {
    const trimmed = stdout.trim();
    if (trimmed === "")
        throw new Error("lark-cli returned empty JSON output.");
    return JSON.parse(trimmed);
}
function asRecord(value) {
    return typeof value === "object" && value !== null && !Array.isArray(value)
        ? value
        : undefined;
}
function readString(record, key) {
    const value = record[key];
    return typeof value === "string" && value.trim() !== "" ? value : undefined;
}
function readNumber(record, key) {
    const value = record[key];
    return typeof value === "number" && Number.isFinite(value) ? value : undefined;
}
function normalizeUsers(payload) {
    const root = asRecord(payload);
    const data = asRecord(root?.data);
    const users = Array.isArray(data?.users) ? data.users : Array.isArray(root?.users) ? root.users : [];
    return users.flatMap((entry) => {
        const record = asRecord(entry);
        const id = record ? readString(record, "open_id") : undefined;
        const name = record ? readString(record, "localized_name") ?? readString(record, "name") : undefined;
        if (!record || !id || !name)
            return [];
        const person = { id, name };
        const email = readString(record, "enterprise_email") ?? readString(record, "email");
        const department = readString(record, "department");
        if (email)
            person.email = email;
        if (department)
            person.department = department;
        return [person];
    });
}
function extractEventId(payload) {
    const root = asRecord(payload);
    const data = asRecord(root?.data) ?? root;
    if (!data)
        return {};
    const event = asRecord(data.event) ?? data;
    const result = {};
    const eventId = readString(event, "event_id") ?? readString(event, "id") ?? readString(data, "event_id");
    const calendarId = readString(event, "calendar_id") ?? readString(data, "calendar_id");
    const link = readString(event, "app_link") ?? readString(event, "url") ?? readString(data, "app_link");
    if (eventId)
        result.eventId = eventId;
    if (calendarId)
        result.calendarId = calendarId;
    if (link)
        result.link = link;
    return result;
}
function extractMessageId(payload) {
    const root = asRecord(payload);
    const data = asRecord(root?.data) ?? root;
    if (!data)
        return undefined;
    const message = asRecord(data.message) ?? data;
    return readString(message, "message_id") ?? readString(message, "id") ?? readString(data, "message_id");
}
export function normalizeAgendaEvents(payload) {
    const root = asRecord(payload);
    const data = asRecord(root?.data);
    const arrays = [
        Array.isArray(payload) ? payload : undefined,
        Array.isArray(root?.data) ? root?.data : undefined,
        Array.isArray(root?.events) ? root?.events : undefined,
        Array.isArray(root?.items) ? root?.items : undefined,
        Array.isArray(root?.event_instances) ? root?.event_instances : undefined,
        Array.isArray(data?.events) ? data?.events : undefined,
        Array.isArray(data?.items) ? data?.items : undefined,
        Array.isArray(data?.event_instances) ? data?.event_instances : undefined,
        Array.isArray(data?.calendar_events) ? data?.calendar_events : undefined,
        Array.isArray(data?.list) ? data?.list : undefined
    ];
    const entries = arrays.find((candidate) => candidate !== undefined) ?? [];
    return entries
        .flatMap((entry, index) => normalizeAgendaEvent(entry, index))
        .sort((left, right) => {
        const leftTime = Date.parse(left.start);
        const rightTime = Date.parse(right.start);
        if (!Number.isFinite(leftTime) && !Number.isFinite(rightTime))
            return left.title.localeCompare(right.title);
        if (!Number.isFinite(leftTime))
            return 1;
        if (!Number.isFinite(rightTime))
            return -1;
        return leftTime - rightTime;
    });
}
function normalizeAgendaEvent(entry, index) {
    const record = asRecord(entry);
    if (!record)
        return [];
    const nestedEvent = asRecord(record.event) ?? asRecord(record.calendar_event) ?? record;
    const start = readDateLike(nestedEvent, "start") ?? readDateLike(nestedEvent, "start_time");
    const end = readDateLike(nestedEvent, "end") ?? readDateLike(nestedEvent, "end_time");
    const title = readString(nestedEvent, "summary") ??
        readString(nestedEvent, "title") ??
        readString(nestedEvent, "subject") ??
        readString(nestedEvent, "name") ??
        "(untitled calendar event)";
    const id = readString(nestedEvent, "event_id") ??
        readString(nestedEvent, "id") ??
        readString(record, "event_id") ??
        `agenda_${index}_${Buffer.from(`${title}:${start ?? ""}`).toString("base64url")}`;
    const normalized = {
        id,
        title,
        start: start ?? "",
        end: end ?? ""
    };
    const calendarId = readString(nestedEvent, "calendar_id") ?? readString(record, "calendar_id");
    const organizerCalendarId = readString(nestedEvent, "organizer_calendar_id") ?? readString(record, "organizer_calendar_id");
    const location = readLocation(nestedEvent.location) ?? readString(nestedEvent, "location");
    const description = readString(nestedEvent, "description") ??
        readString(nestedEvent, "remarks") ??
        readString(nestedEvent, "remark") ??
        readString(nestedEvent, "note");
    const attendeeCount = readNumber(nestedEvent, "attendee_count") ??
        readNumber(nestedEvent, "attendees_count") ??
        (Array.isArray(nestedEvent.attendees) ? nestedEvent.attendees.length : undefined) ??
        (Array.isArray(nestedEvent.participants) ? nestedEvent.participants.length : undefined);
    const normalizedCalendarId = calendarId ?? organizerCalendarId;
    if (normalizedCalendarId)
        normalized.calendarId = normalizedCalendarId;
    if (location)
        normalized.location = location;
    if (description)
        normalized.description = description;
    if (attendeeCount !== undefined)
        normalized.attendeeCount = attendeeCount;
    const notificationTarget = readNotificationTarget(nestedEvent);
    if (notificationTarget)
        normalized.notificationTarget = notificationTarget;
    return [normalized];
}
function readNotificationTarget(record) {
    const target = asRecord(record.notification_target) ?? asRecord(record.notificationTarget);
    const chatId = (target ? readString(target, "chat_id") ?? readString(target, "chatId") : undefined) ??
        readString(record, "chat_id") ??
        readString(record, "chatId");
    const attendeeUserIds = readStringArray(target?.attendee_user_ids ?? target?.attendeeUserIds ?? record.attendee_user_ids ?? record.attendeeUserIds);
    const normalized = {};
    if (chatId)
        normalized.chatId = chatId;
    if (attendeeUserIds.length > 0)
        normalized.attendeeUserIds = attendeeUserIds;
    return normalized.chatId || normalized.attendeeUserIds ? normalized : undefined;
}
function readStringArray(value) {
    if (!Array.isArray(value))
        return [];
    return value.flatMap((entry) => (typeof entry === "string" && entry.trim() !== "" ? [entry.trim()] : []));
}
function readDateLike(record, key) {
    const value = record[key];
    if (typeof value === "string" && value.trim() !== "")
        return value;
    if (typeof value === "number" && Number.isFinite(value)) {
        const millis = value > 10_000_000_000 ? value : value * 1000;
        return new Date(millis).toISOString();
    }
    const nested = asRecord(value);
    if (!nested)
        return undefined;
    const timestamp = readNumber(nested, "timestamp");
    if (timestamp !== undefined) {
        const millis = timestamp > 10_000_000_000 ? timestamp : timestamp * 1000;
        return new Date(millis).toISOString();
    }
    return (readString(nested, "date_time") ??
        readString(nested, "datetime") ??
        readString(nested, "iso") ??
        readString(nested, "date"));
}
function readLocation(value) {
    if (typeof value === "string" && value.trim() !== "")
        return value;
    const record = asRecord(value);
    if (!record)
        return undefined;
    return readString(record, "name") ?? readString(record, "address") ?? readString(record, "display_name");
}
export class LarkCliContactAdapter {
    runner;
    timeoutMs;
    identity;
    constructor(options = {}) {
        this.runner = options.runner ?? createNodeProcessRunner(options.cliPath);
        this.timeoutMs = options.timeoutMs ?? DEFAULT_TIMEOUT_MS;
        this.identity = options.identity ?? "user";
    }
    async resolvePeople(names) {
        const uniqueNames = [...new Set(names.map((name) => name.trim()).filter(Boolean))];
        if (uniqueNames.length === 0)
            return {};
        const result = await this.runner([
            "contact",
            "+search-user",
            "--queries",
            uniqueNames.join(","),
            "--as",
            this.identity,
            "--format",
            "json"
        ], { timeoutMs: this.timeoutMs });
        if (result.code !== 0) {
            throw new Error(result.stderr.trim() || "lark-cli contact lookup failed.");
        }
        const payload = parseJson(result.stdout);
        const users = normalizeUsers(payload);
        const byName = {};
        for (const name of uniqueNames) {
            const candidates = users.filter((person) => person.name === name);
            if (candidates.length === 1 && candidates[0]) {
                byName[name] = { status: "unique", person: candidates[0] };
            }
            else if (candidates.length > 1) {
                byName[name] = { status: "ambiguous", candidates };
            }
            else {
                byName[name] = { status: "missing" };
            }
        }
        return byName;
    }
}
export class LarkCliCalendarAdapter {
    runner;
    timeoutMs;
    identity;
    constructor(options = {}) {
        this.runner = options.runner ?? createNodeProcessRunner(options.cliPath);
        this.timeoutMs = options.timeoutMs ?? DEFAULT_TIMEOUT_MS;
        this.identity = options.identity ?? "user";
    }
    async createEvent(request) {
        const argv = [
            "calendar",
            "+create",
            "--summary",
            request.title,
            "--start",
            request.start,
            "--end",
            request.end,
            "--as",
            this.identity,
            "--format",
            "json"
        ];
        if (request.description)
            argv.push("--description", request.description);
        if (request.attendeeIds.length > 0)
            argv.push("--attendee-ids", request.attendeeIds.join(","));
        const result = await this.runner(argv, { timeoutMs: this.timeoutMs });
        if (result.code !== 0) {
            return {
                ok: false,
                code: "LARK_CALENDAR_CREATE_FAILED",
                message: result.stderr.trim() || "lark-cli calendar creation failed."
            };
        }
        try {
            const event = extractEventId(parseJson(result.stdout));
            const success = {
                ok: true,
                eventId: event.eventId ?? "unknown"
            };
            if (event.calendarId)
                success.calendarId = event.calendarId;
            if (event.link)
                success.link = event.link;
            return success;
        }
        catch (error) {
            return {
                ok: false,
                code: "LARK_CALENDAR_PARSE_FAILED",
                message: error instanceof Error ? error.message : String(error)
            };
        }
    }
    async listEvents(request) {
        const calendarId = request.calendarId ?? "primary";
        const result = await this.runner([
            "calendar",
            "+agenda",
            "--start",
            request.start,
            "--end",
            request.end,
            "--calendar-id",
            calendarId,
            "--as",
            this.identity,
            "--format",
            "json"
        ], { timeoutMs: this.timeoutMs });
        if (result.code !== 0) {
            return {
                ok: false,
                code: "LARK_CALENDAR_LIST_FAILED",
                message: result.stderr.trim() || "lark-cli calendar agenda lookup failed."
            };
        }
        try {
            return {
                ok: true,
                calendarId,
                events: normalizeAgendaEvents(parseJson(result.stdout))
            };
        }
        catch (error) {
            return {
                ok: false,
                code: "LARK_CALENDAR_LIST_PARSE_FAILED",
                message: error instanceof Error ? error.message : String(error)
            };
        }
    }
}
export class LarkCliIMAdapter {
    runner;
    timeoutMs;
    identity;
    constructor(options = {}) {
        this.runner = options.runner ?? createNodeProcessRunner(options.cliPath);
        this.timeoutMs = options.timeoutMs ?? DEFAULT_TIMEOUT_MS;
        this.identity = options.identity ?? "user";
    }
    async sendText(request) {
        const targetArgv = buildMessageTargetArgv(request);
        if (!targetArgv) {
            return {
                ok: false,
                code: "LARK_MESSAGE_TARGET_MISSING",
                message: "A chat id or attendee user id is required to send a Lark message."
            };
        }
        const argv = [
            "im",
            "+messages-send",
            ...targetArgv,
            "--text",
            request.text,
            "--as",
            this.identity,
            "--format",
            "json"
        ];
        if (request.idempotencyKey)
            argv.push("--idempotency-key", request.idempotencyKey);
        const result = await this.runner(argv, { timeoutMs: this.timeoutMs });
        if (result.code !== 0) {
            return {
                ok: false,
                code: "LARK_MESSAGE_SEND_FAILED",
                message: result.stderr.trim() || "lark-cli message send failed."
            };
        }
        try {
            const messageId = extractMessageId(parseJson(result.stdout));
            if (!messageId) {
                return {
                    ok: false,
                    code: "LARK_MESSAGE_PARSE_FAILED",
                    message: "lark-cli message send response did not include a message_id."
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
                code: "LARK_MESSAGE_PARSE_FAILED",
                message: error instanceof Error ? error.message : String(error)
            };
        }
    }
}
function buildMessageTargetArgv(request) {
    if (request.chatId)
        return ["--chat-id", request.chatId];
    const firstUserId = request.attendeeUserIds?.[0];
    if (firstUserId)
        return ["--user-id", firstUserId];
    return undefined;
}
//# sourceMappingURL=lark-cli.js.map