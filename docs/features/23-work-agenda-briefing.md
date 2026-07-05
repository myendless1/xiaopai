# 日程简报

## 职责
在触摸或每日触发时生成简短工作日程简报，包括今日事项和上一工作周回顾。

## 逻辑
1. 根据事件时间和时区计算今天 00:00 到次日 00:00 的窗口。
2. 计算上一周一到周五的 recap 窗口。
3. 调用日历适配器分别读取两个窗口。
4. 用关键词规则分类事件，选取今日高亮事项。
5. 生成简洁 speech、presentation、actions 和 context patch。

## 关键实现
- `openclaw-skills/plugins/work-assistant/src/agenda/assistant.ts`: 窗口计算、日程分类、摘要响应。
- 分类类别: `outdoor_activity`、`customer_reception`、`internal_meeting`、`deep_work`、`uncategorized`。
- `agenda.summary.generate`: 表示本地摘要生成动作。

## 注意点
- 日历读取失败时走 degraded 响应，但仍返回可播报文本。
- `maxHighlights` 默认 3，避免机器人播报过长。
- `context_patch` 记录日期、事件数量、高亮和分类统计。
