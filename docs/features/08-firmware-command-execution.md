# 设备命令执行

## 职责
消费服务器或实时通道下发的命令，并在设备端执行表情、状态、播报、音量、相机、寻人、运动、序列、停止和 OTA 检查。

## 命令类型
- 表情: `face`、`expression`、`action`
- 语音状态: `state`、`device_state`、`sleep`、`wake`、`stop`
- 播报: `speak`
- 音量: `volume`、`sound`
- 视觉: `capture_image`、`track_once`、`camera`、`find_owner`
- 头部运动: `motion`、`move`、`node_head`、`nod_head`
- 组合: `sequence`
- OTA: `check_ota`、`ota_check`、`firmware_ota`

## 关键实现
- `stack-chan/main/main_command_services.inc`: `execute_command_object_internal` 是命令分发核心。
- `run_command_http_loop`: 通过 `/device/next-command` 长轮询拿命令，收到后 ACK 并执行。
- `sequence_step_failure_is_nonfatal`: 摄像头/寻人类步骤失败可继续执行后续步骤。

## 注意点
- `sequence` 会判断是否需要结束后恢复默认表情。
- `speak` 在录音中会入队，避免边录边播。
- 非休眠类命令会调用 `mark_user_interaction`，刷新自动暗屏计时。
