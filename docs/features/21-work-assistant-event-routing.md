# Work Assistant 事件分发

## 职责
把 OpenClaw 传入的规范化 `InputEvent` 分发给对应业务 assistant，并对有写副作用的响应做幂等缓存。

## 支持事件
- `user_utterance`: 日程创建或会议迟到通知。
- `head_touch`、`daily_briefing_triggered`: 日程简报。
- `meeting_starting_soon`: 会议提醒。
- `outdoor_event_detected`、`business_trip_tomorrow_detected`: 出行规划。
- `sedentary_detected`、`wellbeing_companion_requested`: 久坐关怀。

## 关键实现
- `openclaw-skills/plugins/work-assistant/src/contracts.ts`: `InputEvent`、`StructuredResponse`、结构化意图校验。
- `openclaw-skills/plugins/work-assistant/src/handler.ts`: 事件类型判断和业务分发。
- `openclaw-skills/plugins/work-assistant/src/runtime/idempotency.ts`: 内存幂等缓存。
- `openclaw-skills/plugins/work-assistant/src/index.ts`: 插件注册和默认适配器组装。

## 注意点
- 插件本身不做模型级自然语言理解，优先接受 `payload.structured_intent`。
- 只有成功的 `lark.calendar.create` 或 `lark.message.send` 会按 `event_id` 缓存响应。
- `workAssistant.handleEvent` 是 `operator.write`，因为部分路径会创建日程或发消息。
