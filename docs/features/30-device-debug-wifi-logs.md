# 设备 Wi-Fi 日志与录音缓存

## 职责
在 DJI USB 接收器占用设备 USB 时，用 Wi-Fi 承担调试日志和状态事件上报；server 同时维护 `save-recording` 参数，控制小派 listen 音频是否落盘缓存。

## 逻辑
1. 固件启动时安装 `esp_log_set_vprintf` sink，本地 console 仍可按配置输出。
2. 固件后台任务轮询 `GET /device/config?device_id=...`，同步 `wifi_log`、`usb_serial`、`state_events` 和上报间隔。
3. 普通 ESP 日志进入 `debug_events` 队列；`xiaopai-state` 和 `expression-state` 通过统一门面变化时写入结构化状态事件。
4. 固件批量 `POST /device/logs`，server 按设备保留最近内存日志，并追加写入 `<capture-dir>/device-logs/<device-id>.log`。
5. `POST /debug/config` 可运行期修改开关；`save-recording=false` 时 HTTP upload 和实时 WebSocket listen 音频都不再保存。

## API
- `GET /device/config?device_id=...`: 固件拉取配置。
- `POST /debug/config`: 修改配置，支持 `wifi_log`、`usb_serial`、`state_events`、`save-recording`/`save_recording`。
- `GET /debug/config`: 查看当前配置。
- `POST /device/logs`: 固件上报日志和状态事件。
- `GET /device/logs?device_id=...&limit=100`: 查看内存日志缓存；也可直接 `tail -f captures/device-logs/<device-id>.log` 查看可读文本日志。
- `GET /debug/recordings`: 查看已保存的 listen 音频文件元信息。

## 关键实现
- `stack-chan/main/main_wifi_debug.inc`: 配置轮询、日志 sink、批量 POST。
- `stack-chan/main/debug_events.cpp`: 固件内存事件队列。
- `stack-chan/main/xiaopai_state.cpp`: 上报 xiaopai-state 变更。
- `stack-chan/main/expression_state.cpp`: 上报 expression-state 变更。
- `stack-chan/stack-chan-server/src/server.py`: `/device/config`、`/debug/config`、`/device/logs`、`/debug/recordings`。
- `stack-chan/stack-chan-server/src/realtime_server.py`: 实时 listen 音频保存完成后登记 recording metadata。

## 注意点
- `usb_serial` 是固件本地 console 输出开关，不改变 ESP32-S3 USB Host/Device 角色；接 DJI USB Mic 时仍以 Wi-Fi 日志作为主要调试通道。
- `/device/logs` 返回的日志和 recording metadata 是 server 内存缓存，server 重启后清空；设备日志文件保存在 `<capture-dir>/device-logs`，音频文件仍保存在 `<capture-dir>/audio`。
- 旧参数 `--save-audio-uploads` 仍兼容，新语义统一为 `--save-recording`。
