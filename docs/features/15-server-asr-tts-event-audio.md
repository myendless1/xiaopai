# Aliyun ASR/TTS 与事件音频缓存

## 职责
把设备上传的音频转为文本，将要播报的文本转为 PCM/WAV，并为常用事件回复生成本地缓存。

## ASR 逻辑
1. `/upload` 和 `/upload-audio` 接收 WAV 或裸 PCM。
2. WAV 自动读取采样率，裸 PCM 使用默认采样率。
3. 调用 Aliyun NLS ASR，返回识别文本。
4. 根据文本处理音量、唤醒、休眠或转发 OpenClaw。

## TTS 逻辑
1. `/stream-speak` 接收文本和 voice/volume/rate 参数。
2. 先规范化文本并按句切分。
3. 第一段打开 Aliyun TTS 流后立即回传，后续句子并发预取。
4. `/tts/debug` 可输出 PCM 或 WAV，便于调试音色。

## 缓存逻辑
- `/event-audio/<name>.pcm|wav` 按事件名生成或读取缓存。
- 缓存元信息包含文本、voice、采样率、音量、TTS URL 和 appkey，配置变化时自动失效。

## 关键实现
- `stack-chan/stack-chan-server/src/server.py`: `_handle_upload`、`_handle_stream_speak`、`_handle_tts_debug`、`ensure_event_audio_cache`。
- `create_aliyun_nls_token`: 使用 AK/SK 自动创建并刷新 NLS token。
