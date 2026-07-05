# Xiaopai 控制插件

## 职责
为 OpenClaw 提供稳定、校验过的小派控制边界，屏蔽本地 stack-chan HTTP 响应细节。

## Gateway 方法
- `xiaopaiControl.execute`: 执行 `speak`、`face`、`action`、`move`、`sequence`、`stop`。
- `tool.xiaopaiControl.execute`: 同上，兼容 tool 前缀。
- `xiaopaiControl.getHealth`: 读取 `/health`。
- `xiaopaiControl.listDevices`: 读取 `/devices`。

## 关键实现
- `openclaw-skills/plugins/xiaopai-control/src/validation.ts`: 命令校验，限制文本长度、表情/动作 allowlist、移动角度和时长。
- `openclaw-skills/plugins/xiaopai-control/src/http-adapter.ts`: 转换为 stack-chan `POST /command` 或 `GET /command/stop`。
- `openclaw-skills/plugins/xiaopai-control/src/results.ts`: 统一返回 `queued`、`rejected`、`failed`。
- `openclaw-skills/plugins/xiaopai-control/src/dry-run.ts`: 无硬件 dry-run 响应。

## 注意点
- `action` 中的 `node_head`/`nod_head` 会转换为物理点头命令，其余动作映射为 face 表情/动画。
- `sequence` 不允许嵌套 sequence 或 stop。
- `defaultDeviceId` 可在命令未指定设备时自动补齐。
