# 寻人和久坐提醒

## 职责
让小派在唤醒或定时检查时主动寻找人脸，并在持续检测到用户久坐时播报提醒。

## 寻人逻辑
1. 先回到相机 home pose。
2. 当前姿态拍照检测人脸，首轮失败时扫描左、右、低、高。
3. 检测到人脸后计算中心偏差，超过停止像素阈值则调整头部。
4. 最多执行配置轮数，完成后可选择播报回复。

## 久坐逻辑
1. 每 `kSedentaryFindOwnerIntervalMs` 到点后先检查是否处于 `sleep_dark` 休眠模式。
2. 若当前是 active 模式，跳过本轮，不拍照、不检测，也不改变连续命中计数。
3. 仅在休眠模式下执行寻人检测，并只在休眠检测命中时累积次数。
4. 休眠检测连续命中达到 `kSedentaryOwnerHitsRequired` 后播报久坐提醒。
5. 提醒文本优先使用事件音频缓存。

## 关键实现
- `stack-chan/main/main_camera_motion.inc`: `run_find_owner_detection`、`run_find_owner_command`、`run_sedentary_find_owner_loop`。
- `mark_wake_find_owner_pending`: 唤醒状态机触发的寻人请求，避免重复执行。

## 注意点
- `preserve_speech_playback` 可让寻人在唤醒回复播放期间同时进行。
- `wait_for_speech` 用于需要等播报完成再移动的场景。
- 久坐提醒的 active/休眠判断使用 `sleep_dark_is_visible()`；active 模式到点只跳过当前判定，不重置休眠累计。
- 久坐提醒是设备本地视觉策略，OpenClaw 的 wellbeing 插件是另一条基于事件的业务路径。
