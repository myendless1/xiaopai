# Xiaopai 渲染兜底

## 职责
当来自 stack-chan 的 OpenClaw turn 需要小派播报，但 agent 没有显式调用 `xiaopaiControl.execute` 播报时，自动把最终 assistant 文本转成小派 speech 命令。

## 逻辑
1. `before_agent_run` 从 JSON envelope、messages 或 session key 判断是否是 Xiaopai 渲染场景。
2. `after_tool_call` 观察成功的 `xiaopaiControl.execute`，只有 `speak` 或含 `speak` 的 `sequence` 能满足渲染。
3. `before_agent_finalize` 若还未播报，则清理最终文本，构造 `sequence: speak + face calm`。
4. 记录诊断结果，不改变 OpenAI 兼容响应形状。

## 关键实现
- `openclaw-skills/plugins/xiaopai-control/src/render-fallback.ts`: intent 检测、执行观察、文本清理、兜底命令生成。
- `openclaw-skills/plugins/xiaopai-control/src/speech-text.ts`: Markdown 表格、列表、链接、emoji 等转语音文本。
- `openclaw-skills/plugins/xiaopai-control/src/index.ts`: 注册 `before_agent_run`、`after_tool_call`、`before_agent_finalize`、`agent_end` hooks。

## 注意点
- 只处理 `schema: openclaw.stackchan.event.v1` 且 `render.target: xiaopai` 的 turn。
- fallback 文本上限复用 `MAX_SPEECH_LENGTH=500`。
- 明确成功播报后会跳过兜底，动作、表情、移动不会抑制兜底。
