# CoreS3/DJI 音频服务

## 职责
封装 M5Stack CoreS3 的麦克风、扬声器、外置 USB 麦克风输入和 Opus/PCM 播放能力，为实时语音、HTTP 录音上传和 TTS 播放提供统一音频底座。

## 逻辑
1. 初始化 AXP2101、AW9523、AW88298、ES7210 和 I2S 双工通道。
2. 启动 USB host 监听标准 USB Audio Class 输入设备；检测到 DJI Mic/Mic Mini 接收器这类 UAC 录音设备后，打开 AudioStreaming isochronous IN endpoint。
3. 输入侧优先读取 USB 接收器的 16/24/32-bit PCM，降到 16-bit、混成单声道并重采样到 16k；只有外置流已有有效采样时才切换为外置输入，否则继续使用 ES7210 内置麦克风。
4. 输出侧将 PCM 或 Opus 解码结果写入播放队列，再由音频任务送到 AW88298。
5. 音量、停止、等待播放空闲都通过服务接口统一控制。

## 关键实现
- `stack-chan/main/audio/xiaopai_audio_service.cpp`: CoreS3 音频芯片供电、I2S、输入输出设备、播放队列、Opus 解码。
- `stack-chan/main/audio/dji_mic_receiver_input.cpp`: USB host 供电、UAC descriptor 扫描、AudioStreaming 接口 claim、isochronous IN 采样读取、单声道混音和 16k 重采样。
- `stack-chan/main/codec_audio_output.cpp`: 兼容旧调用的薄封装，实际转发到 `audio_service_*`。
- `audio_service_get_input_status()`: 查询当前有效输入源、DJI 接收器是否被检测到、USB 字符串是否确认 DJI 身份、是否已有可用音频流。
- `AudioPlayOptions`: 控制播放是否阻塞、队列满时是否丢弃旧音频。
- `CONFIG_STACKCHAN_DJI_MIC_USB_INPUT`: 控制是否启用 DJI/UAC 外置麦自动切换；fallback 采样率和声道数由 `CONFIG_STACKCHAN_DJI_MIC_USB_ASSUME_RATE`、`CONFIG_STACKCHAN_DJI_MIC_USB_ASSUME_CHANNELS` 设置。

## 注意点
- 输出播放使用 SPIRAM 优先分配音频块，内存不足时回退内部 RAM。
- 音频服务默认按 16k 单声道协议处理，与 Aliyun 和 Xiaozhi 实时链路对齐。
- 外置 USB 麦克风是 fail-safe 接管：设备未插入、descriptor 不匹配、claim 失败或暂时无音频包时，不影响内置麦克风收音。
- DJI 身份优先通过 USB manufacturer/product 字符串确认；若接收器不暴露可识别字符串，但暴露标准 UAC PCM 输入流，则按 UAC 外置麦克风路径兜底接管。
- 录音、TTS 和实时音频共享设备，业务层需用 `audio_mutex` 与 voice pause guard 协调。
