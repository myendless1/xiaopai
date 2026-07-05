# 飞书日程创建

## 职责
根据用户请求创建飞书日程，并解析或校验参会人。

## 逻辑
1. 优先读取 `payload.structured_intent`，要求 `type=calendar.create`、`version=1`。
2. 结构化意图不存在时，使用有限规则解析中文文本中的日期、时间、标题和邀请人。
3. 校验标题、开始/结束时间、参会人和时间顺序。
4. 姓名参会人通过联系人适配器解析，ID 参会人直接使用。
5. 调用日历适配器创建事件，返回 `StructuredResponse`。

## 关键实现
- `openclaw-skills/plugins/work-assistant/src/calendar/assistant.ts`: 输入归一化、校验、联系人解析、创建日程和响应构造。
- `openclaw-skills/plugins/work-assistant/src/calendar/parser.ts`: legacy 文本规则解析。
- `openclaw-skills/plugins/work-assistant/src/calendar/time.ts`: 相对日期和本地时间转换。

## 注意点
- 文本 parser 只支持已知表达模式，不承担自由 NLU。
- 参会人姓名缺失或多义时会返回 follow-up，不会猜测。
- 成功后 `context_patch.last_created_calendar_event` 保存新日程上下文。
