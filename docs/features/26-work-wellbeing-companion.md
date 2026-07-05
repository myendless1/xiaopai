# 久坐关怀陪伴

## 职责
基于外部传入的久坐检测事件生成轻量健康提醒，并支持后续短笑话或放松提示。

## 久坐提醒逻辑
1. 校验 `duration_minutes` 和 `confidence`。
2. 低置信度、时长不足、冷却期内直接 skipped。
3. 读取当前到未来短窗口的日程上下文。
4. 若正在会议中，跳过 audible nudge。
5. 否则生成起身活动提醒，并提供后续陪伴选项。

## 陪伴请求逻辑
- `wellbeing_companion_requested` 根据请求类型选择短笑话、放松提示或轻聊天文本。
- 响应只生成文本和 presentation，不做硬件控制。

## 关键实现
- `openclaw-skills/plugins/work-assistant/src/wellbeing/assistant.ts`: 阈值、冷却、日历上下文、模板选择。
- 默认阈值: 久坐 20 分钟、置信度 0.8、冷却 30 分钟、未来日程窗口 30 分钟。

## 注意点
- 本插件不拍照、不分类姿态、不控制机器人硬件。
- `context_patch.wellbeing_last_nudge_at` 用于下一次冷却判断。
- 日历读取失败时可降级继续提醒，并标记 `wellbeing_calendar_degraded`。
