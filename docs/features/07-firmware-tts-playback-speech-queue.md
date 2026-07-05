# TTS 播放与播报队列

## 职责
把服务器下发的 `speak` 命令或本地回复文本转换为音频播放，并处理打断、队列覆盖、缓存音频和灯带反馈。

## 逻辑
1. `speak` 命令优先进入长度为 1 的 `speak_command_queue`，新命令覆盖旧命令。
2. 播报任务等待录音结束后开始播放。
3. 有 `cache_name` 时从 `/event-audio/<name>.pcm` 取缓存音频，否则从 `/stream-speak` 拉取 Aliyun PCM 流。
4. 播放时启动说话灯带动画，结束后恢复语音状态。

## 关键实现
- `stack-chan/main/main_tts_commands.inc`: `execute_speak_command_internal`、`enqueue_speak_command`、`run_speak_command_loop`、`stream_pcm_url`。
- `request_speak_preempt`: 新播报、用户开口或停止命令会打断当前播放。
- `send_command_ack`: 对服务器命令回传 `received`、`done` 或 `failed`。

## 注意点
- 缓存音频用于唤醒、休眠、久坐等短回复，降低首包延迟。
- `pause_voice_listener` 或后台语音任务存在时，播报期间会暂停麦克风监听。
- 临时兜底话术“我没听清，可以再说一遍吗”被显式静默，避免无意义播报。
