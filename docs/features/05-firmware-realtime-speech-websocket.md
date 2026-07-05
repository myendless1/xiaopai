# 实时语音 WebSocket

## 职责
设备通过 Xiaozhi 兼容 WebSocket 与本地服务器进行实时语音交互：麦克风 PCM 编码为 Opus 上行，服务器返回 STT、设备状态、MCP 工具调用和 Opus TTS 音频。

## 逻辑
1. 从 `/realtime/config` 读取 WebSocket URL，失败时用 HTTP 服务器地址推导端口 `+1`。
2. 连接后发送 hello，等待服务器 hello 并记录 `session_id`。
3. 监听电平超过阈值时发送 `listen start`，持续编码 Opus 帧，静音或超时后发送 `listen stop`。
4. 同时轮询下行消息，执行 MCP/command、显示 STT 文本、播放服务端 Opus TTS。

## 关键实现
- `stack-chan/main/main_realtime_speech.inc`: WebSocket 配置、连接、Opus 编码、录音触发、断线重连。
- `stack-chan/main/main_realtime_transport.inc`: hello/listen 消息、WebSocket 收发、MCP 工具名到本地命令转换。
- `run_xiaozhi_ota_probe()`: 当前后台语音任务入口。

## 注意点
- 语音采样受 `local_voice_can_sample_mic()` 控制，播报和等待状态会自动暂停采样。
- `receive_ws_once` 能处理二进制 Opus 音频，直接交给 `audio_service_play_opus_frame_16k`。
- MCP 工具调用映射到 `face`、`motion`、`find_owner`、`capture_image`、`volume`、`sequence`、`stop`。
