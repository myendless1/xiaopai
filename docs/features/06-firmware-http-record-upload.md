# 本地录音上传 ASR

## 职责
提供非实时的录音上传路径：设备本地 VAD 录音，封装 WAV 后 POST 到服务器 `/upload-audio`。

## 逻辑
1. 后台循环读取麦克风短块，计算平均绝对电平并维护 pre-roll。
2. 电平超过 `CONFIG_STACKCHAN_VOICE_START_THRESHOLD` 后开始录音。
3. 平滑电平低于停止阈值并持续静音，或达到最长录音时间后停止。
4. 将 PCM 加 WAV 头，上传到服务器并解析返回 STT JSON。

## 关键实现
- `stack-chan/main/main_realtime_speech.inc`: `run_local_record_upload_loop`、`record_pcm_after_trigger`、`make_wav_bytes`、`upload_wav_recording`。
- `kPreRollSamples`: 保留触发前音频，避免开头被截断。
- `post_speech_echo_guard_active()`: 防止刚播报完的回声触发录音。

## 注意点
- 当前启动流程主要使用实时 WebSocket；本功能仍保留为 HTTP 兼容路径。
- 录音过程中会设置 `voice_recording_active`，播报命令会排队等录音完成。
- 上传接口带 `X-Device-Id` 和 `X-Client-Id`，服务器据此绑定设备会话。
