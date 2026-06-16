# Work Assistant Plugin

`work-assistant` is the OpenClaw execution boundary for workplace assistant behavior. This MVP exposes `workAssistant.handleEvent`, accepts normalized `InputEvent` objects, and returns `StructuredResponse` objects for robot-facing callers.

The plugin includes five domain handlers:

- `calendar_assistant` validates calendar creation input, resolves Lark attendees, creates a Lark calendar event, and records write-side effects.
- `agenda_briefing` handles read-only morning briefing triggers, lists Lark calendar events, summarizes today, and recaps the previous work week.
- `meeting_reminder` handles scheduler-produced meeting reminders, stores the reminded meeting in `context_patch.current_focus`, and can send user-requested late-arrival notifications through Lark IM when a safe target is known.
- `travel_planner` handles scheduler-produced outdoor visit and next-day business trip reminders, using route/weather/profile adapter boundaries and returning degraded guidance when data is missing.
- `wellbeing_companion` handles sedentary detection and light follow-up companionship without performing robot sensing, hardware control, or Lark writes.

It also includes an optional proactive calendar trigger scheduler. The scheduler is disabled by default; when explicitly enabled, it scans bounded future calendar windows through the configured calendar adapter, evaluates deterministic rules, persists trigger plans, and dispatches normalized `InputEvent` objects back through `workAssistant.handleEvent`. Scheduler scans never call an LLM or model to inspect the calendar.

When `scheduler.agentDispatch.enabled` is configured, a due scheduler trigger first calls the internal assistant handler and obtains the canonical `StructuredResponse`. If that response has speech, the plugin then queues a one-shot OpenClaw agent turn through `api.session.workflow.scheduleSessionTurn`. The scheduled message is a pure `openclaw.stackchan.event.v1` JSON envelope with `render.target: "xiaopai"` and includes both the original scheduler `InputEvent` and the generated `StructuredResponse`. Queueing that agent turn is the scheduler success boundary; Xiaopai speech execution happens later when the agent turn calls `xiaopaiControl.execute` or the Xiaopai fallback hook queues speech.

The companion skill under `skills/work-assistant` is guidance only; it is not the execution layer.

OpenClaw should perform flexible natural-language understanding before calling this plugin. The preferred request includes `payload.structured_intent`; the plugin validates and executes that deterministic intent, but it does not perform model-backed natural-language inference.

## Gateway Method

`workAssistant.handleEvent` accepts either the event object directly or `{ "event": <InputEvent> }`.

Preferred structured calendar creation request:

```json
{
  "event": {
    "event_id": "fixture-calendar-create-structured-001",
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

Structured attendees may use `{ "name": "Gargantua" }` for plugin-owned Lark contact lookup, or `{ "id": "ou_xxx" }`, `{ "id": "oc_xxx" }`, or `{ "id": "omm_xxx" }` to bypass lookup with an already valid Lark attendee identifier.

Legacy text fallback remains available when `payload.structured_intent` is absent:

```json
{
  "event": {
    "event_id": "fixture-calendar-create-001",
    "type": "user_utterance",
    "timestamp": "2026-06-05T10:00:00+08:00",
    "user_id": "ou_requester",
    "payload": {
      "text": "明天上午10点到11点的项目会，帮我建一个飞书日程，邀请张三、李四参会。"
    },
    "context": {
      "timezone": "Asia/Shanghai"
    }
  }
}
```

The fallback parser only supports known wording patterns. For flexible utterances, extract `payload.structured_intent` first and keep the original utterance in `payload.text` for audit/debug context.

Agenda briefing requests use the same `InputEvent` envelope and are triggered by robot/system event types:

```json
{
  "event": {
    "event_id": "fixture-head-touch-001",
    "type": "head_touch",
    "timestamp": "2026-06-06T08:00:00+08:00",
    "user_id": "ou_requester",
    "payload": {},
    "context": {
      "timezone": "Asia/Shanghai"
    }
  }
}
```

`daily_briefing_triggered` is also supported. The response includes concise `speech`, presentation hints, two read-side `lark.calendar.list` actions for today's agenda and the previous Monday-through-Friday recap window, an `agenda.summary.generate` action, `follow_up.expected=false`, and a `context_patch` containing `briefing_date`, `today_event_count`, selected `highlight_events`, and recap `category_counts`.

Meeting reminders are produced by the scheduler with `type: "meeting_starting_soon"` and `payload.calendar_event`. A successful reminder returns a `meeting.reminder.generate` action and stores the meeting for immediate follow-up:

```json
{
  "event": {
    "event_id": "fixture-meeting-reminder-001",
    "type": "meeting_starting_soon",
    "timestamp": "2026-06-06T09:20:00+08:00",
    "user_id": "ou_requester",
    "payload": {
      "trigger": {
        "rule_id": "meeting_starting_soon",
        "scheduled_for": "2026-06-06T01:20:00.000Z",
        "fired_at": "2026-06-06T01:20:00.000Z",
        "source": "proactive_calendar_scheduler",
        "trigger_key": "trigger_meeting_starting_soon_2026_06_06"
      },
      "calendar_event": {
        "id": "dry_today_1",
        "title": "客户来访接待",
        "start": "2026-06-06T09:30:00+08:00",
        "end": "2026-06-06T10:30:00+08:00",
        "calendar_id": "primary",
        "location": "上海办公室",
        "notification_target": {
          "chat_id": "oc_dry_run_customer_visit"
        }
      }
    },
    "context": {
      "timezone": "Asia/Shanghai"
    }
  }
}
```

Late-arrival follow-ups should prefer `payload.structured_intent`:

```json
{
  "event": {
    "event_id": "fixture-meeting-notify-late-001",
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
        "event_id": "dry_today_1",
        "calendar_id": "primary",
        "title": "客户来访接待",
        "start_time": "2026-06-06T09:30:00+08:00",
        "end_time": "2026-06-06T10:30:00+08:00",
        "notification_target": {
          "chat_id": "oc_dry_run_customer_visit"
        }
      }
    }
  }
}
```

The notification handler sends one `lark.message.send` action only when `context.current_focus` identifies a calendar event and has a usable `notification_target.chat_id` or `notification_target.attendee_user_ids`. Missing focus or missing recipients returns a follow-up instead of guessing. Successful message sends are idempotent by `event_id`.

Travel reminders are produced by the scheduler with `type: "outdoor_event_detected"` or `type: "business_trip_tomorrow_detected"` and `payload.calendar_event`. Outdoor reminders use a conservative destination resolver, read origin/preferences through the profile adapter, estimate a route when origin and destination are available, and compute a recommended departure time from event start, route duration, and `arrivalBufferMinutes`.

```json
{
  "event": {
    "event_id": "fixture-outdoor-travel-001",
    "type": "outdoor_event_detected",
    "timestamp": "2026-06-06T17:00:00+08:00",
    "user_id": "ou_requester",
    "payload": {
      "trigger": {
        "rule_id": "outdoor_event",
        "scheduled_for": "2026-06-06T09:00:00.000Z",
        "fired_at": "2026-06-06T09:00:00.000Z",
        "source": "proactive_calendar_scheduler",
        "trigger_key": "trigger_outdoor_event_2026_06_06",
        "calendar_id": "primary",
        "source_event_id": "dry_today_4"
      },
      "calendar_event": {
        "id": "dry_today_4",
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

Successful outdoor reminders return `user.profile.read`, `route.estimate`, and `travel.plan.generate` action records, plus `context_patch.current_focus` with the source event, destination, and recommended departure time when available. Missing destination skips route lookup and records a failed `travel.plan.generate`. Missing origin or route adapter failure keeps the reminder useful but omits precise departure-time guidance.

Business trip reminders use the same scheduler envelope and focus on weather and bounded preparation guidance:

```json
{
  "event": {
    "event_id": "fixture-business-trip-001",
    "type": "business_trip_tomorrow_detected",
    "timestamp": "2026-06-06T18:00:00+08:00",
    "user_id": "ou_requester",
    "payload": {
      "trigger": {
        "rule_id": "business_trip_tomorrow",
        "scheduled_for": "2026-06-06T10:00:00.000Z",
        "fired_at": "2026-06-06T10:00:00.000Z",
        "source": "proactive_calendar_scheduler",
        "trigger_key": "trigger_business_trip_tomorrow_2026_06_06",
        "calendar_id": "primary",
        "source_event_id": "dry_tomorrow_trip"
      },
      "calendar_event": {
        "id": "dry_tomorrow_trip",
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

Successful business-trip reminders return `weather.forecast` and `travel.plan.generate` action records and a `travel_summary` context patch. Weather lookup failure is reported as a failed action while the response still includes destination, event time, and deterministic preparation notes such as documents, charger, and work materials. Travel reminders do not create calendar events, update events, or send Lark messages.

Sedentary detection requests are also normalized `InputEvent` objects. The robot or sensing service provides detection output; the plugin does not capture camera frames, classify posture, move robot hardware, change lights directly, or perform long-term health tracking.

```json
{
  "event": {
    "event_id": "fixture-sedentary-detected-001",
    "type": "sedentary_detected",
    "timestamp": "2026-06-06T13:40:00+08:00",
    "user_id": "ou_requester",
    "payload": {
      "duration_minutes": 35,
      "confidence": 0.91,
      "source": "robot_vision",
      "device_id": "robot-dry-run-001"
    },
    "context": {
      "timezone": "Asia/Shanghai"
    }
  }
}
```

The wellbeing assistant validates `payload.duration_minutes` and `payload.confidence`, applies configurable duration, confidence, cooldown, and upcoming-reminder thresholds, then uses `lark.calendar.list` to check a bounded calendar window. Current meeting overlap returns a quiet skipped response. A nearby upcoming event may be included as one concise reminder. Calendar lookup failure is recorded as a failed `lark.calendar.list` action and the wellbeing decision degrades to payload/context-only evaluation.

Allowed nudges return short robot-speakable `speech`, calm presentation hints, a `wellbeing.sedentary.evaluate` action, `follow_up.expected=true`, and context fields such as `wellbeing_last_nudge_at`, `wellbeing_last_decision`, `wellbeing_follow_up_offered`, and optionally `wellbeing_nearby_event`. Skipped decisions return no audible nudge speech, `follow_up.expected=false`, and a skipped `wellbeing.sedentary.evaluate` action with the decision reason.

Follow-up companionship requests use the same method:

```json
{
  "event": {
    "event_id": "fixture-wellbeing-companion-requested-001",
    "type": "wellbeing_companion_requested",
    "timestamp": "2026-06-06T13:41:00+08:00",
    "user_id": "ou_requester",
    "payload": {
      "content_type": "relaxation"
    },
    "context": {
      "timezone": "Asia/Shanghai",
      "wellbeing_last_nudge_at": "2026-06-06T13:40:00+08:00"
    }
  }
}
```

The follow-up response contains one bounded deterministic joke, relaxation prompt, or light companionship message, positive presentation hints, and a `wellbeing.companion.generate` action. It does not call a model to generate humor.

## Lark Runtime

The default adapters use `lark-cli` with fixed argv arrays and JSON output:

- `lark-cli contact +search-user --queries <names> --as user --format json`
- `lark-cli calendar +create --summary <title> --start <iso> --end <iso> --attendee-ids <ids> --as user --format json`
- `lark-cli calendar +agenda --start <iso> --end <iso> --calendar-id <id> --as user --format json`
- `lark-cli im +messages-send --chat-id <oc_xxx> --text <text> --as user --format json`

Required Lark scopes:

- `contact:user:search`
- `calendar:calendar.event:read`
- `calendar:calendar.event:create`
- `calendar:calendar.event:update`
- `calendar:calendar.free_busy:read` only if a deployment adds free/busy or meeting-time suggestion flows
- `im:message.send_as_user` and `im:message` for user-identity meeting notifications, or `im:message:send_as_bot` for bot-identity sends where the bot has access to the target chat

For the default user-identity runtime, both the app backend and the local user token must have the needed scopes. Check the current user authorization with:

```bash
lark-cli auth status --verify
lark-cli auth check --scope "contact:user:search calendar:calendar.event:read calendar:calendar.event:create calendar:calendar.event:update"
```

Authorize missing optional flows incrementally, for example:

```bash
lark-cli auth login --scope "calendar:calendar.free_busy:read im:message.send_as_user im:message"
```

The default identity is `user`, because `contact +search-user` is user-only. The plugin config supports `larkIdentity`, `larkCliPath`, `timeoutMs`, `dryRun`, optional travel settings, optional wellbeing settings, and optional scheduler settings:

```json
{
  "dryRun": true,
  "travel": {
    "originAddress": "上海办公室",
    "defaultRouteMode": "driving",
    "arrivalBufferMinutes": 15
  },
  "wellbeing": {
    "minimumSedentaryDurationMinutes": 20,
    "minimumConfidence": 0.8,
    "cooldownMinutes": 30,
    "upcomingReminderHorizonMinutes": 30
  }
}
```

Scheduler example:

```json
{
  "dryRun": true,
  "scheduler": {
    "enabled": true,
    "startIntervalLoop": true,
    "scanIntervalMs": 600000,
    "lookaheadHours": 48,
    "timezone": "Asia/Shanghai",
    "userId": "ou_requester",
    "calendarId": "primary",
    "statePath": ".openclaw/work-assistant-scheduler-state.json",
    "agentDispatch": {
      "enabled": true,
      "sessionKey": "xiaopai-device-1",
      "deliveryMode": "none",
      "deviceId": "44:1b:f6:e4:83:8c",
      "interrupt": true
    },
    "rules": {
      "dailyBriefing": {
        "enabled": true,
        "localTime": "08:00"
      },
      "meetingStartingSoon": {
        "enabled": true,
        "offsetMinutes": 10
      },
      "outdoorEvent": {
        "enabled": true,
        "offsetMinutes": 60
      },
      "businessTripTomorrow": {
        "enabled": true,
        "localTime": "18:00"
      }
    }
  }
}
```

Only `dailyBriefing` is enabled by default once the scheduler itself is enabled. `meetingStartingSoon`, `outdoorEvent`, and `businessTripTomorrow` stay disabled unless explicitly configured; enable proactive rules only for deployments ready for robot speech. For travel reminders, configure `travel.originAddress` or use dry-run profile defaults if route-aware outdoor departure times are expected.

`agentDispatch` is also opt-in. Use a Xiaopai/stack-chan session key when the proactive reminder should be handled by the OpenClaw agent/LLM layer and rendered by `xiaopai-control`. The default `deliveryMode` is `none`, so the scheduled agent turn can render voice without also posting a normal chat announcement; set `announce` only when a channel-visible cron reply is desired.

Scheduler-produced events keep the existing `InputEvent` envelope. Their payload includes `payload.trigger` with rule id, scheduled time, fired time, trigger key, calendar id, and source metadata; calendar-derived events also include `payload.calendar_event` with source event id, title, start, end, optional location or description, and optional notification target metadata.

## Dry Run

Set plugin config `dryRun: true` to use mocked contact, calendar, IM, route, weather, and user-profile adapters. Dry run returns deterministic resource IDs, agenda events, recap events, wellbeing calendar context, message ids, travel route/weather/profile data, and scheduler scan fixtures for briefing, meeting, outdoor, and trip plans. It does not call `lark-cli`, map providers, or weather providers.

## Scheduler Watch Script

For terminal-based proactive trigger testing, run:

```bash
npm run watch:scheduler
```

The script scans the configured Lark calendar, prints scheduler trigger plans as they are detected, and dispatches due plans through the local `workAssistant.handleEvent` handler so the terminal shows generated speech and action counts. For safe local verification without Lark calls:

```bash
npm run watch:scheduler -- --dry-run --once
```

For real Lark testing, create calendar events whose title, location, or description contains trigger keywords such as `会议`, `同步`, `客户`, `拜访`, `园区`, `外出`, `出差`, `航班`, or `高铁`. The script defaults to a 2-minute meeting/outdoor offset and uses the current local time for daily briefing and next-day trip reminder rules, so near-term test events can be observed quickly. Use `--no-dispatch` when you only want to print detected plans without invoking handlers.

## Future Direct OpenAPI Adapter

Keep direct OpenAPI support behind the existing `LarkContactAdapter`, `LarkCalendarAdapter`, and `LarkIMAdapter` interfaces. The assistant handler, parser, idempotency, and Gateway method should not depend on the transport.

## Verification

```bash
npm install
npm run verify
openclaw plugins inspect work-assistant --runtime --json
openclaw gateway call workAssistant.handleEvent --params "$(jq -c '{event:.}' fixtures/sample-event.json)"
openclaw gateway call workAssistant.handleEvent --params "$(jq -c '{event:.}' fixtures/sedentary-detected-event.json)"
openclaw gateway call workAssistant.handleEvent --params "$(jq -c '{event:.}' fixtures/sedentary-skipped-cooldown-event.json)"
openclaw gateway call workAssistant.handleEvent --params "$(jq -c '{event:.}' fixtures/wellbeing-companion-requested-event.json)"
openclaw gateway call workAssistant.handleEvent --params "$(jq -c '{event:.}' fixtures/proactive-daily-briefing-triggered-event.json)"
openclaw gateway call workAssistant.handleEvent --params "$(jq -c '{event:.}' fixtures/proactive-meeting-starting-soon-event.json)"
openclaw gateway call workAssistant.handleEvent --params "$(jq -c '{event:.}' fixtures/meeting-notify-late-structured-event.json)"
openclaw gateway call workAssistant.handleEvent --params "$(jq -c '{event:.}' fixtures/proactive-outdoor-event-detected-event.json)"
openclaw gateway call workAssistant.handleEvent --params "$(jq -c '{event:.}' fixtures/proactive-business-trip-tomorrow-detected-event.json)"
openclaw gateway call workAssistant.handleEvent --params "$(jq -c '{event:.}' fixtures/proactive-outdoor-missing-destination-event.json)"
openclaw gateway call workAssistant.handleEvent --params "$(jq -c '{event:.}' fixtures/proactive-outdoor-route-failure-event.json)"
openclaw gateway call workAssistant.handleEvent --params "$(jq -c '{event:.}' fixtures/proactive-business-trip-weather-failure-event.json)"
```

See `VERIFICATION.md` for the local verification results captured for this MVP.
