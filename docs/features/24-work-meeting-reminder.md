# 会议提醒与迟到通知

## 职责
处理调度器产生的会议开始提醒，并在用户跟进时向会议群或参会人发送迟到通知。

## 会议提醒逻辑
1. 接收 `meeting_starting_soon` 事件。
2. 校验 `payload.calendar_event` 必须包含 id/title/start/end。
3. 生成会议提醒 speech。
4. 把会议保存到 `context_patch.current_focus`，供后续通知使用。

## 迟到通知逻辑
1. `user_utterance` 中的 `meeting.notify_late` 结构化意图优先。
2. fallback 仅识别明显“通知/晚到”类表达。
3. 必须从 `context.current_focus` 找到当前会议。
4. 必须有 `notification_target.chat_id` 或 `attendee_user_ids`。
5. 调用 IM 适配器发送文本，使用 `event_id` 做 idempotency key。

## 关键实现
- `openclaw-skills/plugins/work-assistant/src/meeting/assistant.ts`: `handleReminder`、`handleLateNotification`、`shouldRouteToMeetingNotification`。
- `openclaw-skills/plugins/work-assistant/src/lark/adapters.ts`: `LarkIMAdapter` 边界。

## 注意点
- 缺少会议焦点或通知目标时只提问，不猜收件人。
- 发送失败会返回 failed action，并保留 current_focus。
