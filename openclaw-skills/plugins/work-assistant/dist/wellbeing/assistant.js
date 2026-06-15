const DEFAULT_MINIMUM_SEDENTARY_DURATION_MINUTES = 20;
const DEFAULT_MINIMUM_CONFIDENCE = 0.8;
const DEFAULT_COOLDOWN_MINUTES = 30;
const DEFAULT_UPCOMING_REMINDER_HORIZON_MINUTES = 30;
const CONTEXT_LOOKBACK_MINUTES = 5;
const MOVEMENT_TEMPLATES = [
    "你已经坐了一段时间。可以起身走动一分钟，转转肩颈，再把目光移向远处放松一下。",
    "该换个姿势了。建议站起来喝口水，做两次肩颈伸展，再让眼睛离开屏幕片刻。",
    "久坐时间到了。可以花一分钟活动脚踝和手腕，慢慢伸展背部，再看向远处。"
];
const JOKE_TEMPLATES = [
    "短笑话：为什么待办清单很安静？因为它一直在等人把话说完再增加一项。",
    "短笑话：会议纪要说自己很忙，因为每天都在帮大家回忆刚刚发生的事。",
    "短笑话：日历最怕什么？怕大家说随便找个时间，因为它知道没有随便这个时区。"
];
const RELAXATION_TEMPLATES = [
    "我们做一个短放松：吸气四拍，停一拍，再慢慢呼气四拍。做两轮就好。",
    "把肩膀轻轻向后绕两圈，再慢慢放下。接着看向远处，给眼睛十秒空档。",
    "先把双脚踩稳，放松下巴和肩膀，缓慢吸气，再把注意力放回当前这一步。"
];
const LIGHT_CHAT_TEMPLATES = [
    "我在。先把节奏放慢一点，做完这次短休息再继续手头的事。",
    "可以先给自己一分钟空档。回来后从最小的一步继续就好。",
    "收到。我们保持轻一点的节奏，先完成眼前最明确的一件事。"
];
export class WellbeingCompanionAssistant {
    calendarAdapter;
    calendarId;
    minimumSedentaryDurationMinutes;
    minimumConfidence;
    cooldownMinutes;
    upcomingReminderHorizonMinutes;
    constructor(options) {
        this.calendarAdapter = options.calendarAdapter;
        this.calendarId = options.calendarId;
        this.minimumSedentaryDurationMinutes =
            options.minimumSedentaryDurationMinutes ?? DEFAULT_MINIMUM_SEDENTARY_DURATION_MINUTES;
        this.minimumConfidence = options.minimumConfidence ?? DEFAULT_MINIMUM_CONFIDENCE;
        this.cooldownMinutes = options.cooldownMinutes ?? DEFAULT_COOLDOWN_MINUTES;
        this.upcomingReminderHorizonMinutes =
            options.upcomingReminderHorizonMinutes ?? DEFAULT_UPCOMING_REMINDER_HORIZON_MINUTES;
    }
    async handle(event) {
        if (event.type === "wellbeing_companion_requested")
            return this.handleCompanionRequest(event);
        return this.handleSedentaryDetected(event);
    }
    async handleSedentaryDetected(event) {
        const validated = validateSedentaryPayload(event.payload);
        if (!validated.ok) {
            return this.skippedResponse(event, "invalid_payload", {
                reason: "invalid_payload",
                validation_errors: validated.errors
            });
        }
        const payload = validated.value;
        if (payload.confidence < this.minimumConfidence) {
            return this.skippedResponse(event, "low_confidence", {
                reason: "low_confidence",
                duration_minutes: payload.duration_minutes,
                confidence: payload.confidence
            });
        }
        if (payload.duration_minutes < this.minimumSedentaryDurationMinutes) {
            return this.skippedResponse(event, "insufficient_duration", {
                reason: "insufficient_duration",
                duration_minutes: payload.duration_minutes,
                confidence: payload.confidence
            });
        }
        const cooldown = this.evaluateCooldown(event);
        if (cooldown.active) {
            return this.skippedResponse(event, "cooldown", {
                reason: "cooldown",
                duration_minutes: payload.duration_minutes,
                confidence: payload.confidence,
                last_nudge_at: cooldown.lastNudgeAt,
                minutes_since_last_nudge: cooldown.minutesSinceLastNudge
            });
        }
        const calendarContext = await this.listCalendarContext(event);
        if (!calendarContext.degraded && calendarContext.currentEvent) {
            return this.skippedResponse(event, "meeting_overlap", {
                reason: "meeting_overlap",
                duration_minutes: payload.duration_minutes,
                confidence: payload.confidence,
                overlapping_event: summarizeEvent(calendarContext.currentEvent)
            }, [calendarContext.action]);
        }
        const upcomingEvent = calendarContext.degraded ? undefined : calendarContext.upcomingEvent;
        const actions = [calendarContext.action, this.evaluateAction("allowed", {
                reason: "nudge_allowed",
                duration_minutes: payload.duration_minutes,
                confidence: payload.confidence,
                calendar_degraded: calendarContext.degraded,
                nearby_event_id: upcomingEvent?.id
            })];
        const contextPatch = {
            wellbeing_last_nudge_at: event.timestamp,
            wellbeing_last_decision: "allowed",
            wellbeing_follow_up_offered: true
        };
        if (upcomingEvent)
            contextPatch.wellbeing_nearby_event = summarizeEvent(upcomingEvent);
        if (calendarContext.degraded)
            contextPatch.wellbeing_calendar_degraded = true;
        return {
            speech: buildNudgeSpeech(event, upcomingEvent),
            presentation: {
                emotion: "calm",
                motion: "stretch_prompt",
                light: "soft_green"
            },
            actions,
            follow_up: {
                expected: true,
                question: "要不要听个短笑话，或者做一个放松提示？",
                reason: "wellbeing_companion_offer"
            },
            context_patch: contextPatch
        };
    }
    handleCompanionRequest(event) {
        const payload = normalizeCompanionPayload(event.payload);
        const contentType = selectCompanionContentType(payload);
        const speech = selectCompanionSpeech(contentType, event.event_id);
        const continueRequested = payload.continue_requested === true;
        const contextPatch = {
            wellbeing_last_decision: "companion_generated",
            wellbeing_follow_up_offered: continueRequested
        };
        return {
            speech,
            presentation: {
                emotion: "positive",
                motion: "small_nod",
                light: "warm"
            },
            actions: [
                {
                    type: "wellbeing.companion.generate",
                    status: "success",
                    details: {
                        content_type: contentType,
                        bounded: true
                    }
                }
            ],
            follow_up: continueRequested
                ? {
                    expected: true,
                    question: "还想再来一个短放松提示吗？",
                    reason: "wellbeing_companion_continue_requested"
                }
                : {
                    expected: false
                },
            context_patch: contextPatch
        };
    }
    async listCalendarContext(event) {
        const at = Date.parse(event.timestamp);
        const start = new Date(at - minutesToMs(CONTEXT_LOOKBACK_MINUTES)).toISOString();
        const end = new Date(at + minutesToMs(this.upcomingReminderHorizonMinutes)).toISOString();
        const request = { start, end };
        if (this.calendarId)
            request.calendarId = this.calendarId;
        const result = await this.calendarAdapter.listEvents(request);
        if (!result.ok) {
            return {
                degraded: true,
                action: {
                    type: "lark.calendar.list",
                    status: "failed",
                    error: {
                        code: result.code,
                        message: result.message
                    },
                    details: {
                        window: "wellbeing_context",
                        start,
                        end
                    }
                }
            };
        }
        const currentEvent = selectCurrentEvent(result.events, at);
        const upcomingEvent = selectUpcomingEvent(result.events, at, this.upcomingReminderHorizonMinutes);
        const details = {
            window: "wellbeing_context",
            start,
            end,
            calendar_id: result.calendarId ?? this.calendarId ?? "primary",
            event_count: result.events.length
        };
        if (currentEvent)
            details.current_event_id = currentEvent.id;
        if (upcomingEvent)
            details.upcoming_event_id = upcomingEvent.id;
        const context = {
            degraded: false,
            action: {
                type: "lark.calendar.list",
                status: "success",
                details
            }
        };
        if (currentEvent)
            context.currentEvent = currentEvent;
        if (upcomingEvent)
            context.upcomingEvent = upcomingEvent;
        return context;
    }
    evaluateCooldown(event) {
        const lastNudgeAt = readContextString(event.context, "wellbeing_last_nudge_at")
            ?? readContextString(event.context, "last_wellbeing_nudge_at");
        if (!lastNudgeAt)
            return { active: false };
        const last = Date.parse(lastNudgeAt);
        const current = Date.parse(event.timestamp);
        if (!Number.isFinite(last) || !Number.isFinite(current))
            return { active: false };
        const minutesSinceLastNudge = (current - last) / 60000;
        if (minutesSinceLastNudge >= 0 && minutesSinceLastNudge < this.cooldownMinutes) {
            return {
                active: true,
                lastNudgeAt,
                minutesSinceLastNudge: Math.round(minutesSinceLastNudge * 10) / 10
            };
        }
        return { active: false };
    }
    skippedResponse(event, decision, details, precedingActions = []) {
        return {
            speech: "",
            presentation: {
                emotion: "quiet",
                motion: "none",
                light: "none"
            },
            actions: [...precedingActions, this.evaluateAction(decision, details)],
            follow_up: {
                expected: false
            },
            context_patch: {
                wellbeing_last_decision: decision,
                wellbeing_follow_up_offered: false
            }
        };
    }
    evaluateAction(decision, details) {
        return {
            type: "wellbeing.sedentary.evaluate",
            status: decision === "allowed" ? "success" : "skipped",
            details: {
                decision,
                minimum_duration_minutes: this.minimumSedentaryDurationMinutes,
                minimum_confidence: this.minimumConfidence,
                cooldown_minutes: this.cooldownMinutes,
                upcoming_reminder_horizon_minutes: this.upcomingReminderHorizonMinutes,
                ...details
            }
        };
    }
}
function validateSedentaryPayload(payload) {
    const errors = [];
    const duration = payload.duration_minutes;
    const confidence = payload.confidence;
    if (typeof duration !== "number" || !Number.isFinite(duration) || duration < 0) {
        errors.push("payload.duration_minutes must be a non-negative number.");
    }
    if (typeof confidence !== "number" || !Number.isFinite(confidence) || confidence < 0 || confidence > 1) {
        errors.push("payload.confidence must be a number between 0 and 1.");
    }
    if (errors.length > 0)
        return { ok: false, errors };
    const value = {
        ...payload,
        duration_minutes: duration,
        confidence: confidence
    };
    const source = readPayloadString(payload, "source");
    const deviceId = readPayloadString(payload, "device_id");
    if (source)
        value.source = source;
    if (deviceId)
        value.device_id = deviceId;
    return { ok: true, value };
}
function normalizeCompanionPayload(payload) {
    const normalized = {};
    const requestType = readPayloadString(payload, "request_type");
    const contentType = readPayloadString(payload, "content_type");
    const continueRequested = payload.continue_requested;
    if (requestType)
        normalized.request_type = requestType;
    if (contentType)
        normalized.content_type = contentType;
    if (continueRequested === true)
        normalized.continue_requested = true;
    return normalized;
}
function selectCompanionContentType(payload) {
    const raw = `${payload.content_type ?? ""} ${payload.request_type ?? ""}`.toLocaleLowerCase();
    if (/relax|breath|calm|stretch|放松|呼吸|伸展/.test(raw))
        return "relaxation";
    if (/chat|companion|陪伴|聊/.test(raw))
        return "light_chat";
    return "joke";
}
function selectCompanionSpeech(contentType, eventId) {
    const bank = contentType === "relaxation"
        ? RELAXATION_TEMPLATES
        : contentType === "light_chat"
            ? LIGHT_CHAT_TEMPLATES
            : JOKE_TEMPLATES;
    return bank[stableIndex(eventId, bank.length)] ?? bank[0] ?? "";
}
function buildNudgeSpeech(event, upcomingEvent) {
    const parts = [MOVEMENT_TEMPLATES[stableIndex(event.event_id, MOVEMENT_TEMPLATES.length)] ?? MOVEMENT_TEMPLATES[0] ?? ""];
    if (upcomingEvent) {
        parts.push(`另外，${formatLocalTime(upcomingEvent.start, event.context.timezone)} 有 ${upcomingEvent.title}。`);
    }
    parts.push("要不要听个短笑话，或者做一个放松提示？");
    return parts.join("");
}
function selectCurrentEvent(events, at) {
    return events
        .filter((event) => {
        const start = Date.parse(event.start);
        const end = Date.parse(event.end);
        return Number.isFinite(start) && Number.isFinite(end) && start <= at && at < end;
    })
        .sort((left, right) => Date.parse(left.start) - Date.parse(right.start))[0];
}
function selectUpcomingEvent(events, at, horizonMinutes) {
    const horizon = at + minutesToMs(horizonMinutes);
    return events
        .filter((event) => {
        const start = Date.parse(event.start);
        return Number.isFinite(start) && start > at && start <= horizon;
    })
        .sort((left, right) => Date.parse(left.start) - Date.parse(right.start))[0];
}
function summarizeEvent(event) {
    const summary = {
        event_id: event.id,
        title: event.title,
        start: event.start,
        end: event.end
    };
    if (event.calendarId)
        summary.calendar_id = event.calendarId;
    return summary;
}
function formatLocalTime(value, timezone) {
    const date = new Date(value);
    if (!Number.isFinite(date.getTime()))
        return value;
    try {
        return new Intl.DateTimeFormat("zh-CN", {
            timeZone: timezone,
            hour: "2-digit",
            minute: "2-digit",
            hour12: false
        }).format(date);
    }
    catch {
        return value;
    }
}
function readContextString(context, key) {
    const value = context[key];
    return typeof value === "string" && value.trim() !== "" ? value : undefined;
}
function readPayloadString(payload, key) {
    const value = payload[key];
    return typeof value === "string" && value.trim() !== "" ? value.trim() : undefined;
}
function stableIndex(seed, length) {
    if (length <= 0)
        return 0;
    let hash = 0;
    for (let index = 0; index < seed.length; index += 1) {
        hash = (hash * 31 + seed.charCodeAt(index)) >>> 0;
    }
    return hash % length;
}
function minutesToMs(minutes) {
    return minutes * 60 * 1000;
}
//# sourceMappingURL=assistant.js.map