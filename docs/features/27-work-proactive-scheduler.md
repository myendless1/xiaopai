# 主动日程触发调度

## 职责
定期扫描未来日程，按确定性规则生成提醒计划，到期后派发成 `InputEvent`，并可进一步排入 Xiaopai agent turn。

## 规则
- `daily_briefing`: 每天固定本地时间触发。
- `meeting_starting_soon`: 会议开始前 N 分钟触发。
- `outdoor_event`: 外出/客户拜访等关键词事件前触发。
- `business_trip_tomorrow`: 明天出差类事件，在当天固定时间触发。

## 关键实现
- `openclaw-skills/plugins/work-assistant/src/scheduler/rules.ts`: 根据日程生成 `TriggerPlan`。
- `openclaw-skills/plugins/work-assistant/src/scheduler/scheduler.ts`: `refresh` 扫描、`dispatchDue` 派发、`tick` 组合执行。
- `openclaw-skills/plugins/work-assistant/src/scheduler/store.ts`: 内存或 JSON 文件存储 pending/dispatched 记录。
- `openclaw-skills/plugins/work-assistant/src/scheduler/agent-dispatch.ts`: 将 assistant 响应包成 `openclaw.stackchan.event.v1` envelope 并调度 agent turn。

## 注意点
- 调度器默认关闭，需 `scheduler.enabled=true`。
- 扫描规则不调用 LLM 判断日程。
- `updateGroupKey` 会替换同组未派发旧计划，避免日程变化后重复提醒。
- agent dispatch 成功边界是 turn 被排队，小派实际播报由后续 agent 或渲染兜底完成。
