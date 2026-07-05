# 本地语音状态机

## 职责
统一管理设备语音交互状态，决定麦克风是否可采样、灯带/表情如何显示，以及播报结束后回到哪个状态。

## 状态
- `Idle`: 休眠或暗屏，允许采样唤醒词。
- `Listening`: 正常监听。
- `Waiting`: OpenClaw 或服务器正在处理。
- `Speaking`: 本地或实时 TTS 正在播报。

## 关键实现
- `stack-chan/main/voice_state.cpp`: 状态切换、嵌套 speaking depth、generation 变更和输出钩子。
- `local_voice_can_sample_mic()`: 只有 `Idle` 与 `Listening` 允许麦克风采样。
- `local_voice_begin_speaking()` / `local_voice_end_speaking()`: 播报期间挂起其他状态变更，播完回到 `return_state`。

## 注意点
- `generation` 用于中断正在录音的循环，避免状态切换后继续上传旧音频。
- `Idle -> Listening` 可触发寻人钩子，唤醒后自动看向用户。
- 状态输出钩子由 `app_main` 注入，实际动作包括灯带、表情和寻人请求。
