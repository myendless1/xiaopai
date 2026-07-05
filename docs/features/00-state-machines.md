# 机器人与表情状态机

## 职责
作为固件侧核心状态说明，明确两套状态机: `xiaopai-state` 管机器人语音/行为状态，`expression-state` 管屏幕表情状态。两者独立读写，但在唤醒、等待、播报和休眠场景中协同变化。

## xiaopai-state
- `Idle`: 休眠或暗屏，允许采样唤醒词。
- `Listening`: 正常监听。
- `Waiting`: OpenClaw 或服务器正在处理。
- `Speaking`: 本地或实时 TTS 正在播报。

## expression-state
- `calm`/`default`: active 监听时的默认表情。
- `thinking`: 等待服务器或 OpenClaw 结果。
- `sleep_dark`: 休眠暗屏表情，也是久坐检测允许执行的状态。
- `shy`、`smile`、`happy_dynamic` 等: 触摸、命令或业务事件触发的临时/动态表情。

## 统一入口
- 获取 xiaopai-state: `xiaopai_state_get()`。
- 修改 xiaopai-state: `xiaopai_state_set(LocalVoiceState::Listening, reason)` 或 `xiaopai_state_set("listening", reason)`。
- 获取 expression-state: `expression_state_get()`。
- 修改 expression-state: `expression_state_set("sleep_dark")`。
- 临时 expression-state: `expression_state_set_temporary("shy", duration_ms)`。
- 播报生命周期: `xiaopai_state_begin_speaking(reason)` / `xiaopai_state_end_speaking(reason)`，用于保留 speaking depth 和播报结束后的返回状态。
- 业务代码不直接调用 `local_voice_*`、`show_expression()` 或 `sleep_dark_is_visible()`；这些只保留在 `xiaopai_state.cpp`、`expression_state.cpp` 和底层实现内。
- 两个入口会在状态实际变化时写入 `debug_events` 队列，由 Wi-Fi debug 任务上报到 server 的 `/device/logs`。

## 快照字段
- xiaopai-state: `name`、`state`、`generation`、`is_speaking`、`can_sample_mic`。
- expression-state: `name`、`screen_visible`、`sleep_dark`、`animation_active`。

## 转换逻辑
1. WebSocket 连接、触摸唤醒或服务端状态命令把机器人切到 `Listening`，通常同时把表情切回 `calm`。
2. 思考/等待命令把机器人切到 `Waiting`，表情可切到 `thinking`。
3. TTS 播放通过 begin/end speaking 进入 `Speaking`，播完回到进入播报前的状态。
4. 空闲超时或 sleep 命令显示 `sleep_dark`；触摸、语音活动或显式唤醒会恢复默认表情。
5. 表情状态可独立变化，但业务层必须分别通过 `xiaopai_state` 和 `expression_state` 读写，避免状态来源分裂。

## 关键实现
- `stack-chan/main/xiaopai_state.cpp`: xiaopai-state 门面，封装机器人状态读写。
- `stack-chan/main/expression_state.cpp`: expression-state 门面，封装表情状态读写。
- `stack-chan/main/debug_events.cpp`: 缓存状态变化和固件日志，供 Wi-Fi debug 任务批量上报。
- `stack-chan/main/main_wifi_debug.inc`: 轮询 `/device/config`，并把 `xiaopai-state`、`expression-state` 变化发到 `/device/logs`。
- `stack-chan/main/voice_state.cpp`: 机器人状态切换、speaking depth、generation 和输出钩子。
- `stack-chan/main/expression_controller.cpp`: 表情绘制、动态表情、临时表情和当前表情快照。

## 注意点
- `generation` 用于中断正在录音的循环，避免状态切换后继续上传旧音频。
- `Idle -> Listening` 只切换监听态输出，不再自动触发寻人，避免拖慢唤醒体验。
- 状态输出钩子由 `app_main` 注入，实际动作包括灯带和表情。
