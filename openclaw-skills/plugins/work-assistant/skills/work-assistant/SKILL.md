---
name: work-assistant
description: Call the local Work Assistant OpenClaw plugin when a robot or workplace user event needs business execution, including Lark calendar creation, read-only agenda briefings, scheduler meeting reminders, travel reminders, meeting late-arrival notifications, and wellbeing events.
---

# Work Assistant

Use this skill as guidance only. The `work-assistant` plugin is the execution layer and owns side effects.

## Execution shortcut

Do not search the filesystem to find the plugin implementation or method name. The Gateway method is fixed:

```bash
openclaw gateway call workAssistant.handleEvent --params '<compact-json-with-event>' --json
```

Use this command directly after constructing the `InputEvent`. Searching with `rg`, `grep`, or `find` is unnecessary for normal execution and can be very slow in OpenClaw agent workspaces.

## Stack-Chan JSON Event Envelopes

When a user message is JSON with `schema: "openclaw.stackchan.event.v1"` and an `event` object, treat it as a stack-chan robot event envelope. The schema identifies structured robot input; it does not mean every event must call `workAssistant.handleEvent`. Do not parse the envelope as ordinary prose, do not invent a new Gateway method, and do not control Xiaopai hardware from this skill.

First inspect `envelope.event.type`, `envelope.event.payload`, and any structured intent. Call `workAssistant.handleEvent` only when the event matches this plugin's business capabilities, such as calendar creation, explicit agenda briefing triggers, scheduler meeting reminders, scheduler travel reminders, late-arrival notifications, sedentary-care events, or wellbeing follow-up events. Ordinary chat, simple Q&A, direct robot expression requests, raw device events such as a bare `head_touch`, and Xiaopai presentation-only commands can be handled by the OpenClaw agent directly and then rendered with `xiaopaiControl.execute`.

If `envelope.event.type` is `work_assistant_proactive_response` and `envelope.event.payload.schema` is `openclaw.work_assistant.scheduler_response.v1`, this is already the output of the work-assistant built-in scheduler. Do not call `workAssistant.handleEvent` again. Treat `envelope.event.payload.structured_response` as the canonical `StructuredResponse`, use its `speech` as the reminder text, preserve any `context_patch` that is useful for follow-up turns, and render the response through `xiaopaiControl.execute` when robot output is needed. The envelope's `render.target: "xiaopai"` also allows the Xiaopai runtime fallback to queue speech if the agent returns final text but forgets the explicit control call.

For events that do match work-assistant, route the embedded event through the existing method:

```bash
openclaw gateway call workAssistant.handleEvent --params '{"event":<envelope.event>}' --json
```

Use exactly `{ "event": <envelope.event> }` as the method params. The returned value is the canonical `StructuredResponse` for that business-handled robot turn. Preserve `envelope.device_id` and `envelope.render` for the later Xiaopai rendering step, but do not pass those outer envelope fields into `workAssistant.handleEvent` unless they are already present in `event.context` or `event.payload`.

If the `StructuredResponse.context_patch` is non-empty, preserve it for later turns in the same stack-chan device session when possible. The stack-chan session is keyed by the stable OpenClaw session header for the device, and later `InputEvent.context` can include relevant context such as `current_focus`, meeting state, or wellbeing follow-up fields.

When the utterance refers to "me", "myself", or the current user as the requester or attendee, first get the current Lark user id with this read-only command:

```bash
lark-cli auth status --verify
```

Use the returned `userOpenId` as `event.user_id` and, when the user wants to invite themself, as a direct attendee id in `structured_intent.attendees`.

When the user asks the workplace robot to create a Lark calendar event, first extract a deterministic `calendar.create` structured intent from the utterance. Then call the Gateway method `workAssistant.handleEvent` with both the original text and `payload.structured_intent`.

For example, for:

```text
明天上午10点到10点半有个活动 OpenClaw 测试，帮我建一个飞书日程，邀请 Gargantua 参会
```

OpenClaw should resolve the relative date from the event timestamp/timezone and pass:

```json
{
  "event": {
    "event_id": "stable-idempotency-key",
    "type": "user_utterance",
    "timestamp": "2026-06-05T10:00:00+08:00",
    "user_id": "ou_requester",
    "payload": {
      "text": "明天上午10点到10点半有个活动 OpenClaw 测试，帮我建一个飞书日程，邀请 Gargantua 参会",
      "structured_intent": {
        "type": "calendar.create",
        "version": "1",
        "title": "OpenClaw 测试",
        "start": "2026-06-06T10:00:00+08:00",
        "end": "2026-06-06T10:30:00+08:00",
        "attendees": [
          { "name": "Gargantua" }
        ]
      }
    },
    "context": {
      "timezone": "Asia/Shanghai"
    }
  }
}
```

Use attendee names when the plugin should resolve people through Lark contacts. Use direct attendee IDs only when an already valid Lark attendee identifier is available: `ou_`, `oc_`, or `omm_`.

If `payload.structured_intent` is absent, the plugin can fall back to its legacy text parser for older callers, but that parser only supports known wording patterns. Do not rely on the plugin for flexible natural-language inference; the plugin validates and executes structured intent and returns follow-ups for incomplete or invalid data.

The method returns a `StructuredResponse` with speech, presentation hints, action records, follow-up state, and context patches. Do not call Lark calendar/contact APIs directly for this workflow unless the plugin is unavailable.

Concrete calendar creation call shape:

```bash
openclaw gateway call workAssistant.handleEvent --params '{"event":{"event_id":"stable-idempotency-key","type":"user_utterance","timestamp":"2026-06-05T10:00:00+08:00","user_id":"ou_requester","payload":{"text":"明天上午10点到10点半有个活动 OpenClaw 测试，帮我建一个飞书日程，邀请 Gargantua 参会","structured_intent":{"type":"calendar.create","version":"1","title":"OpenClaw 测试","start":"2026-06-06T10:00:00+08:00","end":"2026-06-06T10:30:00+08:00","attendees":[{"name":"Gargantua"}]}},"context":{"timezone":"Asia/Shanghai"}}}' --json
```

For agenda briefing, call the same Gateway method with an explicit robot/system trigger event. Prefer `daily_briefing_triggered`. Do not infer agenda briefing from a bare `head_touch`; `head_touch` is only a generic device input unless upstream context or payload explicitly says it represents an agenda briefing request.

Proactive `daily_briefing_triggered` events should come from the plugin's deterministic scheduler output, not ad hoc LLM calendar polling. The scheduler scans bounded calendar windows through calendar adapters, creates stored trigger plans, and dispatches normalized `InputEvent` objects with `payload.trigger`. Do not repeatedly ask a model to inspect the user's calendar for proactive reminders.

```json
{
  "event": {
    "event_id": "stable-briefing-event-id",
    "type": "daily_briefing_triggered",
    "timestamp": "2026-06-06T08:00:00+08:00",
    "user_id": "ou_requester",
    "payload": {},
    "context": {
      "timezone": "Asia/Shanghai"
    }
  }
}
```

The plugin derives today's agenda window and the previous Monday-through-Friday recap window from `timestamp` and `context.timezone`, lists calendar events, classifies them deterministically, and returns concise robot speech. Agenda briefing is read-only: expected actions are `lark.calendar.list` and `agenda.summary.generate`, never `lark.calendar.create`.

Scheduler-produced `meeting_starting_soon` events are handled by the plugin when the scheduler rule is explicitly enabled. Pass the event through unchanged; do not re-read the calendar or generate reminder text outside the plugin.

```json
{
  "event": {
    "event_id": "stable-meeting-reminder-id",
    "type": "meeting_starting_soon",
    "timestamp": "2026-06-06T09:20:00+08:00",
    "user_id": "ou_requester",
    "payload": {
      "trigger": {
        "rule_id": "meeting_starting_soon",
        "scheduled_for": "2026-06-06T01:20:00.000Z",
        "fired_at": "2026-06-06T01:20:00.000Z",
        "source": "proactive_calendar_scheduler",
        "trigger_key": "trigger_xxx"
      },
      "calendar_event": {
        "id": "event_xxx",
        "title": "项目同步",
        "start": "2026-06-06T09:30:00+08:00",
        "end": "2026-06-06T10:00:00+08:00",
        "calendar_id": "primary",
        "location": "线上会议",
        "notification_target": {
          "chat_id": "oc_xxx"
        }
      }
    },
    "context": {
      "timezone": "Asia/Shanghai"
    }
  }
}
```

The reminder response may include `context_patch.current_focus`. Preserve that context in the next user turn so follow-up utterances like "我会晚到五分钟，帮我通知参会人" can refer to the meeting implicitly.

For late-arrival notifications, prefer a deterministic `meeting.notify_late` intent:

```json
{
  "event": {
    "event_id": "stable-late-notification-id",
    "type": "user_utterance",
    "timestamp": "2026-06-06T09:25:00+08:00",
    "user_id": "ou_requester",
    "payload": {
      "text": "我会晚到五分钟，帮我通知参会人",
      "structured_intent": {
        "type": "meeting.notify_late",
        "version": "1",
        "delay_minutes": 5,
        "message": "我会晚五分钟到，请大家稍等一下"
      }
    },
    "context": {
      "timezone": "Asia/Shanghai",
      "current_focus": {
        "type": "calendar_event",
        "event_id": "event_xxx",
        "calendar_id": "primary",
        "title": "项目同步",
        "start_time": "2026-06-06T09:30:00+08:00",
        "end_time": "2026-06-06T10:00:00+08:00",
        "notification_target": {
          "chat_id": "oc_xxx"
        }
      }
    }
  }
}
```

Only send a late-notification request when `current_focus` identifies the meeting. The plugin will ask a follow-up if focus or recipients are missing, and it will protect successful `lark.message.send` actions with the provided `event_id`.

Scheduler-produced travel events are handled by the plugin when the travel scheduler rules are explicitly enabled. Pass `outdoor_event_detected` and `business_trip_tomorrow_detected` events through unchanged; do not re-read the calendar, call map/weather tools directly, or generate travel speech outside the plugin.

Outdoor travel reminder shape:

```json
{
  "event": {
    "event_id": "stable-outdoor-travel-id",
    "type": "outdoor_event_detected",
    "timestamp": "2026-06-06T17:00:00+08:00",
    "user_id": "ou_requester",
    "payload": {
      "trigger": {
        "rule_id": "outdoor_event",
        "scheduled_for": "2026-06-06T09:00:00.000Z",
        "fired_at": "2026-06-06T09:00:00.000Z",
        "source": "proactive_calendar_scheduler",
        "trigger_key": "trigger_xxx"
      },
      "calendar_event": {
        "id": "event_xxx",
        "title": "外出客户园区拜访",
        "start": "2026-06-06T18:00:00+08:00",
        "end": "2026-06-06T19:30:00+08:00",
        "calendar_id": "primary",
        "location": "客户园区",
        "description": "带方案材料到现场沟通"
      }
    },
    "context": {
      "timezone": "Asia/Shanghai"
    }
  }
}
```

The plugin returns a concise robot-speakable reminder and read-only/advisory actions such as `user.profile.read`, `route.estimate`, and `travel.plan.generate`. Preserve `context_patch.current_focus` when present; it may include a travel event focus and recommended departure time. Missing destination, missing origin, or route failure are degraded in the response and should not be fixed by guessing.

Business trip reminder shape:

```json
{
  "event": {
    "event_id": "stable-business-trip-id",
    "type": "business_trip_tomorrow_detected",
    "timestamp": "2026-06-06T18:00:00+08:00",
    "user_id": "ou_requester",
    "payload": {
      "trigger": {
        "rule_id": "business_trip_tomorrow",
        "scheduled_for": "2026-06-06T10:00:00.000Z",
        "fired_at": "2026-06-06T10:00:00.000Z",
        "source": "proactive_calendar_scheduler",
        "trigger_key": "trigger_xxx"
      },
      "calendar_event": {
        "id": "trip_xxx",
        "title": "北京出差",
        "start": "2026-06-07T09:00:00+08:00",
        "end": "2026-06-07T20:00:00+08:00",
        "calendar_id": "primary",
        "location": "北京",
        "description": "航班和客户会议"
      }
    },
    "context": {
      "timezone": "Asia/Shanghai"
    }
  }
}
```

Business trip responses include `weather.forecast` and `travel.plan.generate` action records and bounded preparation guidance. Weather failures degrade to preparation-only guidance; do not ask the user a follow-up for scheduler metadata defects.

Travel responses include presentation hints only. Xiaopai rendering remains a later rendering step owned by the OpenClaw agent or `xiaopaiControl.execute`; do not make Work Assistant call robot motion, light, or expression APIs directly.

For sedentary-care robot events, call the same Gateway method with `type: "sedentary_detected"`. The plugin expects fixed sensing output and does not perform camera capture, posture recognition, robot movement, light control, or long-term health tracking.

```json
{
  "event": {
    "event_id": "stable-sedentary-event-id",
    "type": "sedentary_detected",
    "timestamp": "2026-06-06T13:40:00+08:00",
    "user_id": "ou_requester",
    "payload": {
      "duration_minutes": 35,
      "confidence": 0.91,
      "source": "robot_vision",
      "device_id": "robot-001"
    },
    "context": {
      "timezone": "Asia/Shanghai",
      "wellbeing_last_nudge_at": "2026-06-06T13:05:00+08:00"
    }
  }
}
```

If a robot integration can only send a fixed-format text message, normalize it before calling the plugin. For example:

```text
SEDENTARY_DETECTED|event_id=robot-evt-123|timestamp=2026-06-06T13:40:00+08:00|user_id=ou_requester|duration_minutes=35|confidence=0.91|source=robot_vision|device_id=robot-001
```

Convert that message into the structured `InputEvent` above. Preserve the robot-provided event id for idempotency, parse numeric fields as numbers, and pass session context such as `wellbeing_last_nudge_at` when available. Do not send this fixed-format text as `payload.text` and expect the plugin to parse it.

When the user accepts the offered companionship turn, call:

```json
{
  "event": {
    "event_id": "stable-wellbeing-follow-up-id",
    "type": "wellbeing_companion_requested",
    "timestamp": "2026-06-06T13:41:00+08:00",
    "user_id": "ou_requester",
    "payload": {
      "content_type": "relaxation"
    },
    "context": {
      "timezone": "Asia/Shanghai",
      "wellbeing_last_nudge_at": "2026-06-06T13:40:00+08:00",
      "wellbeing_last_decision": "allowed",
      "wellbeing_follow_up_offered": true
    }
  }
}
```

Supported follow-up `content_type` values are `joke`, `relaxation`, and `light_chat`. The response is deterministic and bounded for robot playback.

MCP is out of scope for this MVP. If an external MCP client needs this capability later, add an adapter that forwards the same `InputEvent` contract into the plugin handler.
