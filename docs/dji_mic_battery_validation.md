# DJI Mic 内置电池模式验证

## 固定条件

- Stack-chan 使用机身 550 mAh 电池，不连接两个 USB-C 或侧面 Grove 外部电源。
- DJI 接收器预先充满电，再连接 CoreS3 主 USB-C。
- 固件使用 UART0 或 Wi-Fi 日志；USB Serial/JTAG 已关闭，避免与 USB OTG Host 共用 PHY。
- 不关闭 ESP32 brownout 检测。
- 首次测试从 Stack-chan 满电开始，并记录接收器型号、固件版本和环境温度。

## 自动检查

在固件目录执行：

```bash
python3 tools/validate_dji_audio_pipeline.py
. ./env.sh
idf.py build
```

FIR 检查必须确认 48 kHz 输入严格产生 16 kHz 输出，4 kHz 通带误差不超过
0.2 dB，9 kHz 抑制至少 35 dB，12 kHz 抑制至少 60 dB。固件构建必须零错误。

## 阶段 A：电源链

1. 不接 DJI，启动屏幕、Wi-Fi、内置麦，记录 `battery_mv`、`battery_percent`、
   `battery_current_ma` 和复位原因。
2. 接入 DJI，确认启动延迟后取得 USB 枚举租约。
3. 枚举期间确认舵机、摄像头与高音量输出被拒绝或限幅。
4. 确认 VBUS 依次经历关闭、Host 启动、打开和稳定，最终约为 5 V。
5. 运行 10 分钟，要求无异常发热、无 brownout、无 USB overcurrent。

## 阶段 B：UAC 与音频质量

1. 使用独立 UAC 录音模式采集 48 kHz、24-bit、双声道 WAV。
2. 确认 VID/PID 为 `2ca3:4011`，格式错误计数为 0。
3. 普通固件中等待连续 800 ms PCM 后才允许切到 DJI。
4. 比较 `left`、`right`、`mix` 和 `auto_once`；量产配置使用一次性自动选声道。
5. 检查 ASR 音频无明显混叠，`raw_drops`、`pcm_drops` 在稳定负载下保持 0。
6. 分别标定内置麦和 DJI 的 start/stop VAD 阈值，不共用增益参数。

## 阶段 C：热插拔与输入一致性

依次验证：

- 无 DJI 启动，始终使用内置麦。
- 启动前已插 DJI，稳定后在 Monitoring 状态切换。
- 空闲时插入和拔出 DJI。
- 录音中插入 DJI：当前句保持原输入，下一安全点才切换。
- 录音中拔出 DJI：`source_lost=true`，终止当前句，不拼接内置麦。
- 播放中插拔 DJI：只更新 pending source，播放结束后切换。
- 接入错误 USB Audio 设备：身份不匹配，不接管输入。
- 连续热插拔 20 次：无死锁、无持续 VBUS、无内存持续下降。

每次切换都核对 `audio_source_generation` 单调递增，pre-roll 不包含上一代输入。

## 阶段 D：逐项负载

每项至少运行 10 分钟，并记录最低电池电压、VBUS、电池峰值电流、UAC drops、
Wi-Fi 断线次数、最小 internal/DMA/PSRAM heap 和复位原因：

1. DJI + 屏幕/触摸。
2. DJI + Wi-Fi 上下行。
3. DJI + 摄像头采集与上传。
4. DJI + 低、中、高音量 TTS。
5. DJI + 单舵机。
6. DJI + 双舵机。
7. DJI + TTS + 摄像头 + 双舵机。

组合负载的通过条件是无 brownout、无 overcurrent、无持续掉帧、命令 ACK 正常。
若第 7 项不稳定，应保留电源租约和负载互斥，不得通过降低 brownout 灵敏度规避。

## 阶段 E：低电与续航

1. 从满电运行到 30%，确认低于启动线时不新启 DJI。
2. 电池电压连续低于 3.55 V 或进入低电状态后，确认 DJI 停止、VBUS 关闭，
   摄像头和舵机被阻止，音量与 RGB 被限制。
3. 确认恢复需要超过 150 mV 滞回并保持多个采样周期，不发生反复抖动。
4. 电池电压持续低于 3.40 V 时确认安全关机。
5. 人为制造一次 brownout 后重启，确认进入安全模式：禁用 DJI、摄像头和舵机，
   保留屏幕、Wi-Fi 与内置麦。
6. 记录满电到低电回退的实际续航；550 mAh 约 2 Wh，只报告实测值。

## 心跳验收字段

必须能够看到：

- `audio_input`、`audio_input_pending`、`audio_source_generation`、`audio_source_lost`
- `dji_detected`、`dji_identity_confirmed`、`dji_capture_ready`
- `dji_raw_drops`、`dji_pcm_drops`、`dji_format_errors`
- `battery_mv`、`battery_percent`、`battery_current_ma`、`vbus_mv`
- `usb_output`、`usb_enumeration_lease`、`dji_power_allowed`、`power_state`
- internal/DMA/PSRAM 当前与最低余量、`reset_reason`、`brownout_boot_count`

