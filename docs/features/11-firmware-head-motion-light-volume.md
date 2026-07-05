# 舵机、灯带与音量控制

## 职责
控制小派头部 yaw/pitch、RGB 灯带状态和扬声音量，作为语音交互和命令执行的物理输出。

## 逻辑
1. 通过 PY32 I2C 扩展器打开舵机 VM 电源和 RGB 灯带引脚。
2. 通过 UART 发送 SCS 舵机写指令，按角度换算为原始位置。
3. 灯带支持静态颜色和双侧柱状电平显示。
4. 音量以 10-100 百分比保存，映射到音频服务输出音量。

## 关键实现
- `stack-chan/main/main_camera_motion.inc`: `enable_servo_power`、`move_head_to_tracking_angles`、`run_node_head_command`、灯带 PY32 写入函数。
- `stack-chan/main/main_tts_commands.inc`: `execute_volume_command`、说话/监听电平到灯带柱状显示。
- `kServoYawZeroRaw`、`kServoPitchZeroRaw`、`kServoStepsPerDegree`: 角度到舵机原始值的校准参数。

## 注意点
- yaw/pitch 会被限制在安全角度范围内。
- 灯带初始化失败会标记 `light_strip_missing` 并延迟重试，不阻塞主功能。
- 音量命令执行后会播报当前百分比。
