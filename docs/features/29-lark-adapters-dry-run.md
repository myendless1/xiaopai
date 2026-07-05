# Lark 适配与 dry-run

## 职责
为 Work Assistant 提供飞书联系人、日历、IM 的适配边界，并支持无外部系统的 deterministic dry-run。

## 适配接口
- `LarkContactAdapter.resolvePeople`: 根据姓名解析人员。
- `LarkCalendarAdapter.createEvent`: 创建日程。
- `LarkCalendarAdapter.listEvents`: 查询日程窗口。
- `LarkIMAdapter.sendText`: 发送文本消息。

## 关键实现
- `openclaw-skills/plugins/work-assistant/src/lark/adapters.ts`: 统一类型和接口。
- `openclaw-skills/plugins/work-assistant/src/lark/lark-cli.ts`: 通过 `lark-cli` 调用真实飞书能力，并规范化输出。
- `openclaw-skills/plugins/work-assistant/src/lark/dry-run.ts`: 固定联系人、日程和消息结果，用于测试和演示。
- `openclaw-skills/plugins/work-assistant/src/index.ts`: 根据 `dryRun` 选择真实适配器或 dry-run 适配器。

## 注意点
- 真实适配器通过 `spawn` 调 `lark-cli`，使用 timeout 防止挂起。
- dry-run 的日程 fixture 同时覆盖今日简报、会议提醒、外出和出差场景。
- `larkIdentity` 可配置为 `user` 或 `bot`，默认 `user`。
