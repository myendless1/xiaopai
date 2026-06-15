import { followUpResponse, validateStructuredAssistantIntent } from "../contracts.js";
import { compareIso } from "./time.js";
import { CalendarIntentParser } from "./parser.js";
export class CalendarAssistant {
    contactAdapter;
    calendarAdapter;
    parser;
    constructor(options) {
        this.contactAdapter = options.contactAdapter;
        this.calendarAdapter = options.calendarAdapter;
        this.parser = options.parser ?? new CalendarIntentParser();
    }
    async handle(event) {
        const normalized = normalizeCalendarInput(event, this.parser);
        if ("response" in normalized)
            return normalized.response;
        const intent = normalized.intent;
        const validation = validateIntent(intent, event);
        if (validation)
            return validation;
        const resolutions = intent.attendeeNames.length > 0 ? await this.contactAdapter.resolvePeople(intent.attendeeNames) : {};
        const attendeeIssue = validateAttendeeResolutions(resolutions);
        if (attendeeIssue)
            return attendeeIssue;
        const resolvedAttendeeIds = intent.attendeeNames.map((name) => {
            const resolution = resolutions[name];
            if (!resolution || resolution.status !== "unique")
                throw new Error(`Unexpected unresolved attendee: ${name}`);
            return resolution.person.id;
        });
        const attendeeIds = [...intent.attendeeIds, ...resolvedAttendeeIds];
        const createResult = await this.calendarAdapter.createEvent({
            title: intent.title,
            start: intent.start,
            end: intent.end,
            requesterId: event.user_id,
            attendeeIds
        });
        if (!createResult.ok) {
            return {
                speech: "日程没有创建成功，我已经记录了失败原因。",
                presentation: {
                    emotion: "concerned",
                    motion: "look_at_user",
                    light: "amber"
                },
                actions: [
                    {
                        type: "lark.calendar.create",
                        status: "failed",
                        error: {
                            code: createResult.code,
                            message: createResult.message
                        }
                    }
                ],
                follow_up: {
                    expected: false
                },
                context_patch: {}
            };
        }
        const action = {
            type: "lark.calendar.create",
            status: "success",
            resource_id: createResult.eventId,
            details: {
                calendar_id: createResult.calendarId ?? "primary",
                attendee_ids: attendeeIds,
                start: intent.start,
                end: intent.end,
                title: intent.title,
                link: createResult.link
            }
        };
        return {
            speech: `好的，已经为你创建好${intent.title}日程，并邀请参会人员了。`,
            presentation: {
                emotion: "happy",
                motion: "nod",
                light: "blink"
            },
            actions: [action],
            follow_up: {
                expected: false
            },
            context_patch: {
                last_created_calendar_event_id: createResult.eventId,
                last_created_calendar_event: {
                    event_id: createResult.eventId,
                    calendar_id: createResult.calendarId ?? "primary",
                    title: intent.title,
                    start: intent.start,
                    end: intent.end,
                    attendee_ids: attendeeIds
                }
            }
        };
    }
}
function normalizeCalendarInput(event, parser) {
    if (Object.prototype.hasOwnProperty.call(event.payload, "structured_intent")) {
        const validation = validateStructuredAssistantIntent(event.payload.structured_intent);
        if (!validation.ok) {
            return {
                response: followUpResponse(structuredIntentQuestion(validation.reason), validation.reason, "结构化日程请求不完整或无效，请补充后再试。")
            };
        }
        if (validation.value.type !== "calendar.create") {
            return {
                response: followUpResponse("这个结构化请求不是日程创建请求，请改为 calendar.create 日程创建请求。", "unsupported_intent_type")
            };
        }
        return {
            intent: fromStructuredIntent(validation.value)
        };
    }
    return {
        intent: fromParserIntent(parser.parse(event))
    };
}
function fromStructuredIntent(intent) {
    return {
        title: intent.title,
        start: intent.start,
        end: intent.end,
        attendeeNames: intent.attendees.flatMap((attendee) => "name" in attendee ? [attendee.name] : []),
        attendeeIds: intent.attendees.flatMap((attendee) => "id" in attendee ? [attendee.id] : []),
        confidence: 1,
        source: "structured"
    };
}
function fromParserIntent(intent) {
    const normalized = {
        attendeeNames: intent.attendeeNames,
        attendeeIds: [],
        confidence: intent.confidence,
        source: "parser"
    };
    if (intent.title)
        normalized.title = intent.title;
    if (intent.start)
        normalized.start = intent.start;
    if (intent.end)
        normalized.end = intent.end;
    return normalized;
}
function structuredIntentQuestion(reason) {
    switch (reason) {
        case "unsupported_intent_type":
            return "这个结构化请求类型暂不支持，请改为 calendar.create 日程创建请求。";
        case "unsupported_intent_version":
            return "这个结构化日程请求版本暂不支持，请使用 version 为 1 的格式。";
        case "missing_required_structured_fields":
            return "结构化日程请求缺少标题、开始时间、结束时间或参会人，请补充完整。";
        case "invalid_time_range":
            return "结束时间需要晚于开始时间，请重新确认时间范围。";
        case "invalid_attendee_reference":
            return "参会人需要提供姓名，或提供 ou_、oc_、omm_ 开头的飞书参会人 ID。";
        default:
            return "结构化日程请求格式无效，请检查后重试。";
    }
}
function validateIntent(intent, event) {
    if (!intent.title) {
        return followUpResponse("我还没理解要创建哪个日程，请补充日程标题、时间和参会人。", "unsupported_utterance");
    }
    if (!intent.start || !intent.end) {
        return followUpResponse("请告诉我这个日程的开始和结束时间。", "missing_time");
    }
    if (intent.confidence < 0.75) {
        return followUpResponse("我还没理解要创建哪个日程，请补充日程标题、时间和参会人。", "unsupported_utterance");
    }
    if (compareIso(intent.start, intent.end) >= 0) {
        return followUpResponse("结束时间需要晚于开始时间，请重新确认时间范围。", "invalid_time_range");
    }
    if (!event.user_id) {
        return followUpResponse("我需要确认发起人身份后才能创建日程。", "missing_requester");
    }
    if (intent.attendeeNames.length === 0 && intent.attendeeIds.length === 0) {
        return followUpResponse("请告诉我要邀请哪些参会人。", "missing_attendees");
    }
    return undefined;
}
function validateAttendeeResolutions(resolutions) {
    const missing = Object.entries(resolutions)
        .filter(([, resolution]) => resolution.status === "missing")
        .map(([name]) => name);
    if (missing.length > 0) {
        return followUpResponse(`没有找到 ${missing.join("、")}，请确认参会人姓名。`, "missing_attendee");
    }
    const ambiguous = Object.entries(resolutions).find(([, resolution]) => resolution.status === "ambiguous");
    if (ambiguous) {
        const [name, resolution] = ambiguous;
        const choices = resolution.status === "ambiguous"
            ? resolution.candidates.map((person) => person.department ? `${person.name}（${person.department}）` : person.name).join("、")
            : "";
        return followUpResponse(`${name} 有多个匹配结果，请确认要邀请哪一位：${choices}`, "ambiguous_attendee");
    }
    return undefined;
}
//# sourceMappingURL=assistant.js.map