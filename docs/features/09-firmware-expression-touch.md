# 表情与触摸交互

## 职责
在屏幕上绘制小派表情，处理触摸唤醒和害羞反馈，并在空闲时进入暗屏表情。

## 逻辑
1. 表情名称映射为内部 `FaceKind`，再派生眼睛、眉毛、嘴巴、脸颊等几何参数。
2. 动态表情通过独立任务按帧切换。
3. 触摸屏幕时根据语音状态决定唤醒、忽略或显示临时 `shy`。
4. 空闲超过阈值后显示 `sleep_dark`。

## 关键实现
- `stack-chan/main/expression_controller.cpp`: 表情绘制、眨眼任务、动态表情、临时表情。
- `stack-chan/main/main_head_touch.inc`: 触摸轮询、触摸事件队列、触摸唤醒和害羞表情。
- `kHeadTouchShyExpressionMs`: 触摸害羞表情持续时间。

## 注意点
- 表情坐标以 320x240 画布为基准，偏移被限制在安全范围内。
- `sleep_dark` 下触摸只唤醒屏幕和监听，不强制播报。
- `Waiting` 状态下触摸被忽略，避免打断 OpenClaw 思考反馈。
