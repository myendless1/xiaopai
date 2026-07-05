# HTTP 命令队列

## 职责
本地服务器作为设备命令中心，接收 OpenClaw、浏览器或测试脚本发来的命令，并通过 HTTP 长轮询或实时 WebSocket 投递到在线设备。

## 逻辑
1. `POST /command` 或 `GET /command/<type>` 规范化命令。
2. 若目标设备有实时 WebSocket 会话，优先实时发送。
3. 否则放入对应设备的 `DeviceCommandQueue`。
4. 设备通过 `/device/next-command` 长轮询取命令，并通过 `/device/ack` 回报状态。

## 关键实现
- `stack-chan/stack-chan-server/src/server.py`: `Handler._handle_command`、`_enqueue_command`、`_handle_next_command`、`_handle_ack`。
- `DeviceCommandQueue`: 支持优先级、TTL、interrupt preempt、coalesce、可丢弃命令和最大队列长度。
- `make_command`: 生成统一命令结构，填充 `cmd_id`、priority、TTL、discardable、coalesce_key。

## 注意点
- `device_id` 省略或为占位符时，服务器选择第一个在线 HTTP 设备或实时设备。
- `speak` 和 `sequence` 会做语音文本规范化和缓存音频名补充。
- 本地 HTTP API 没有认证，只适合可信局域网或 loopback。
