# 功能文档索引

本目录按功能点拆分项目文档。每篇只记录功能逻辑、关键实现和主要入口，便于查代码和后续维护。

## 核心状态机
- [机器人与表情状态机](00-state-machines.md)

## 固件与设备侧
- [固件启动与后台服务](01-firmware-boot-background-services.md)
- [WiFi 配网与服务器选择](02-wifi-provisioning-server-selection.md)
- [CoreS3/DJI 音频服务](04-firmware-audio-service.md)
- [实时语音 WebSocket](05-firmware-realtime-speech-websocket.md)
- [本地录音上传 ASR](06-firmware-http-record-upload.md)
- [TTS 播放与播报队列](07-firmware-tts-playback-speech-queue.md)
- [设备命令执行](08-firmware-command-execution.md)
- [表情与触摸交互](09-firmware-expression-touch.md)
- [相机上传与视觉跟踪](10-firmware-camera-vision-tracking.md)
- [舵机、灯带与音量控制](11-firmware-head-motion-light-volume.md)
- [寻人和久坐提醒](12-firmware-find-owner-sedentary.md)
- [固件 OTA 升级](13-firmware-ota.md)

## 本地服务器
- [HTTP 命令队列](14-server-http-command-queue.md)
- [Aliyun ASR/TTS 与事件音频缓存](15-server-asr-tts-event-audio.md)
- [Xiaozhi 实时桥接](16-server-realtime-xiaozhi-bridge.md)
- [OpenClaw 事件路由](17-server-openclaw-routing.md)
- [图像转换、人脸检测与跟踪命令](18-server-image-face-tracking.md)
- [设备 Wi-Fi 日志与录音缓存](30-device-debug-wifi-logs.md)

## OpenClaw 插件与业务能力
- [Xiaopai 控制插件](19-openclaw-xiaopai-control-plugin.md)
- [Xiaopai 渲染兜底](20-openclaw-render-fallback.md)
- [Work Assistant 事件分发](21-work-assistant-event-routing.md)
- [飞书日程创建](22-work-calendar-create.md)
- [日程简报](23-work-agenda-briefing.md)
- [会议提醒与迟到通知](24-work-meeting-reminder.md)
- [出行规划](25-work-travel-planner.md)
- [久坐关怀陪伴](26-work-wellbeing-companion.md)
- [主动日程触发调度](27-work-proactive-scheduler.md)
- [天气 Provider](28-weather-provider.md)
- [Lark 适配与 dry-run](29-lark-adapters-dry-run.md)
