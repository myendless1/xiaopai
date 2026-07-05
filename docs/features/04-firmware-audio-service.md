# CoreS3 音频服务

## 职责
封装 M5Stack CoreS3 的麦克风、扬声器和 Opus/PCM 播放能力，为实时语音、HTTP 录音上传和 TTS 播放提供统一底座。

## 逻辑
1. 初始化 AXP2101、AW9523、AW88298、ES7210 和 I2S 双工通道。
2. 输入侧从 ES7210 读取硬件采样，必要时线性重采样到协议采样率。
3. 输出侧将 PCM 或 Opus 解码结果写入播放队列，再由音频任务送到 AW88298。
4. 音量、停止、等待播放空闲都通过服务接口统一控制。

## 关键实现
- `stack-chan/main/audio/xiaopai_audio_service.cpp`: CoreS3 音频芯片供电、I2S、输入输出设备、播放队列、Opus 解码。
- `stack-chan/main/codec_audio_output.cpp`: 兼容旧调用的薄封装，实际转发到 `audio_service_*`。
- `AudioPlayOptions`: 控制播放是否阻塞、队列满时是否丢弃旧音频。

## 注意点
- 输出播放使用 SPIRAM 优先分配音频块，内存不足时回退内部 RAM。
- 音频服务默认按 16k 单声道协议处理，与 Aliyun 和 Xiaozhi 实时链路对齐。
- 录音、TTS 和实时音频共享设备，业务层需用 `audio_mutex` 与 voice pause guard 协调。
