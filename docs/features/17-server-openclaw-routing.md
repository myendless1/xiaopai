# OpenClaw 事件路由

## 职责
把设备事件和 ASR 文本转发给 OpenClaw，同时在等待期间给设备下发思考态反馈。

## 逻辑
1. ASR 有文本且会话已唤醒时，构造 `speech_recognition` 事件。
2. 非语音设备事件通过 `/device/event` 进入，转换为简短自然语言事件文本。
3. 服务器先给设备下发 `state: waiting` 和 `face: thinking`。
4. 异步调用 OpenClaw `/chat/completions`，设置系统提示和稳定 session key。
5. OpenClaw 侧通过插件或 HTTP 命令 API 再控制小派。

## 关键实现
- `stack-chan/stack-chan-server/src/server.py`: `_send_openclaw_event`、`_call_openclaw`、`build_openclaw_event_text`。
- `stack-chan/stack-chan-server/src/openclaw_agent.py`: OpenClaw chat 调用和回复文本提取。
- `stack-chan/stack-chan-server/src/xiaopai_openclaw_prompt.py`: 小派系统提示。

## 注意点
- `head_touch` 和 `touch` 当前本地处理为 `face: shy`，不会转发 OpenClaw。
- 空语音事件直接跳过，避免无效 OpenClaw 调用。
- session key 格式为 `<prefix>-<device_id>`，按设备保持对话上下文。
