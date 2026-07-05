# Xiaozhi 实时桥接

## 职责
提供 Xiaozhi 兼容 WebSocket 服务，将设备 Opus 上行音频桥接到 Aliyun 流式 ASR，并把命令、MCP 调用和 TTS/LLM 状态发回设备。

## 逻辑
1. HTTP 服务通过 `/realtime/config` 暴露 WebSocket 地址、token 和 OTA 信息。
2. 设备连接 `/xiaozhi/ws` 后，服务器发送 hello 和 `device_state: listening`。
3. 二进制 Opus 帧由 `OpusCodec` 解码为 PCM，进入 `RealtimeAsrBridge`。
4. ASR partial/final 文本通过 `stt` 消息回设备。
5. final 文本按唤醒/休眠/睡眠态/OpenClaw 规则处理。
6. 控制命令被转换为 MCP `tools/call` 发到设备，播报命令则构造含 `speak` 的 sequence。

## 关键实现
- `stack-chan/stack-chan-server/src/realtime_server.py`: `RealtimeManager`、`RealtimeDeviceSession`、`RealtimeAsrBridge`。
- `stack-chan/stack-chan-server/src/aliyun_streaming_asr.py`: Aliyun 流式 ASR WebSocket 协议封装。
- `stack-chan/stack-chan-server/src/aliyun_streaming_tts.py`: Aliyun 流式 TTS 客户端。
- `stack-chan/stack-chan-server/src/xiaozhi_protocol.py`: hello、stt、llm、tts、mcp 消息构造。

## 注意点
- 实时会话内维护 `dialog_awake`，未唤醒时普通文本会被忽略。
- 睡眠词会先播短回复，再发送 `device_state: sleep`。
- 当前 OpenClaw 实时回复只发送 LLM 文本提示，实际播报倾向通过命令或兜底渲染完成。
