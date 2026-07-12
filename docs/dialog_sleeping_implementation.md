# 小派对话休眠与唤醒：当前完整实现

## 1. 文档目的

本文记录小派当前已经落地的“对话休眠”实现，包括设备端状态机、服务端过滤逻辑、WebSocket 协议、告别语播放、重连恢复、显示与灯带行为、测试覆盖及已知限制。

这里的“对话休眠”不是 ESP32 Deep Sleep，也不是本地离线唤醒。当前实现的准确含义是：

- 屏幕显示 `sleep_dark`，灯带关闭；
- 麦克风、VAD、录音、WebSocket 和云端 ASR 继续工作；
- 服务端在休眠状态下只接受唤醒词；
- 普通识别结果、命令文本和重复休眠词均被忽略；
- 服务端确认唤醒词后，设备才恢复正常显示和对话。

因此，该状态应理解为“对话层休眠”，而不是硬件低功耗休眠。

## 2. 设计目标

当前实现满足以下行为：

1. 用户在正常对话中说“拜拜”等休眠词，设备只回复一次告别语。
2. 告别语播放结束后，设备保持黑屏，不恢复 `calm`。
3. 休眠期间仍继续采集语音，以便识别唤醒词。
4. 休眠期间的普通语音不会点亮屏幕，也不会提交给 Morrow。
5. 休眠期间再次说“拜拜”不会再次回复。
6. 只有服务端识别到唤醒词并发送 `listening` 后，设备才退出休眠。
7. WebSocket 短暂断开和重连不会无条件唤醒设备。
8. 普通的空闲超时黑屏仍保持原语义，不会被误认为对话休眠。

## 3. 两种黑屏状态

当前代码中存在两种外观相似、语义不同的黑屏。

| 场景 | 本地语音状态 | 表情 | 服务端对话状态 | 普通语音行为 |
| --- | --- | --- | --- | --- |
| 空闲超时黑屏 | `Listening` | `sleep_dark` | 通常仍为 awake | 普通语音可直接开始正常对话 |
| 对话休眠 | `DialogSleeping` | `sleep_dark` | `dialog_awake=False` | 继续识别，但服务端只接受唤醒词 |

空闲超时黑屏由 `show_sleep_dark_listening()` 实现。它只是关闭显示，底层仍是普通 `Listening`。

对话休眠由 `show_dialog_sleeping()` 实现。它切换到独立的 `LocalVoiceState::DialogSleeping`。

相关文件：

- `stack-chan/main/main_app_state.inc`
- `stack-chan/main/main.cpp`
- `stack-chan/main/main_realtime_transport.inc`
- `stack-chan/main/main_command_services.inc`

## 4. 设备端状态机

### 4.1 `LocalVoiceState::DialogSleeping`

设备端在 `stack-chan/main/voice_state.h` 中新增了稳定状态：

```cpp
enum class LocalVoiceState : uint8_t {
    Idle,
    Listening,
    DialogSleeping,
    Waiting,
    Speaking,
};
```

`DialogSleeping` 的核心属性如下：

| 属性 | 行为 |
| --- | --- |
| 是否允许采集麦克风 | 是 |
| `InteractionState` | `Monitoring` |
| 屏幕表情 | `sleep_dark` |
| 灯带 | 关闭 |
| 普通声音是否点亮屏幕 | 否 |
| 播放完成后的返回状态 | `DialogSleeping` |

`local_voice_can_sample_mic()` 将 `DialogSleeping` 与 `Idle`、`Listening` 一样视为可采集状态：

```cpp
return state == LocalVoiceState::Idle ||
       state == LocalVoiceState::Listening ||
       state == LocalVoiceState::DialogSleeping;
```

在 `stack-chan/main/xiaopai_state.cpp` 中，`DialogSleeping` 被映射为 `InteractionState::Monitoring`。因此 supervisor 不会阻止音频输入任务，休眠期间仍能完成 VAD、录音和 ASR 上传。

### 4.2 独立输出钩子

`LocalVoiceStateHooks` 新增了 `set_dialog_sleeping`。设备初始化时把它绑定到 `set_dialog_sleeping_outputs()`：

```cpp
static void set_dialog_sleeping_outputs()
{
    light_strip_listening_after_speech = false;
    set_light_strip_sleeping();
    expression_state_set("sleep_dark");
}
```

该输出函数保证：

- 不允许语音播放结束后恢复监听灯效；
- 灯带保持关闭；
- 屏幕保持 `sleep_dark`。

普通 `Listening` 仍调用 `set_listening_outputs()`，恢复监听灯效和默认表情。两者不再共用输出逻辑。

### 4.3 播放告别语后的返回状态

语音状态机在开始播放时保存原状态：

```text
DialogSleeping -> Speaking
```

`local_voice_return_state_after_speaking()` 只把 `Speaking` 或 `Waiting` 归一化为 `Listening`，不会改写 `DialogSleeping`。因此播放结束后状态为：

```text
Speaking -> DialogSleeping
```

随后重新应用 `set_dialog_sleeping_outputs()`，屏幕和灯带继续保持关闭。

这解决了原来的错误路径：

```text
Listening + sleep_dark
-> Speaking
-> Listening
-> set_listening_outputs()
-> calm
```

### 4.4 服务端状态消息映射

设备在 `stack-chan/main/main_realtime_transport.inc` 中处理 WebSocket `device_state` 消息：

```text
sleep / sleeping / idle -> DialogSleeping
wake / awake / listen / listening -> Listening
wait / waiting / thinking -> Waiting
speak / speaking -> Speaking
```

HTTP/轮询命令通道中的 `state`、`device_state` 和 `sleep` 命令也映射到同一个 `DialogSleeping` 状态，避免两个控制通道产生不同语义。

## 5. 休眠期间的监听行为

### 5.1 VAD 和录音继续运行

实时语音循环仍会在 `DialogSleeping` 状态下：

1. 读取麦克风数据；
2. 计算音量阈值；
3. VAD 命中后发送 `listen:start`；
4. 编码并发送 Opus 音频；
5. 发送 `listen:stop`；
6. 等待服务端 STT 和状态消息。

也就是说，当前唤醒词仍由服务端 ASR 识别，不是设备端本地关键词检测。

### 5.2 普通声音不点亮屏幕

正常监听状态下，VAD 命中会调用：

```cpp
mark_user_interaction("voice activity");
```

该函数会把 `sleep_dark` 恢复成默认表情。当前实现增加了状态保护：

```cpp
if (xiaopai_state_get().state != LocalVoiceState::DialogSleeping) {
    mark_user_interaction("voice activity");
}
```

因此，休眠期间检测到普通声音时仍会录音识别，但屏幕不会在得到识别结果前提前点亮。

### 5.3 监听灯效保持关闭

`update_listening_light_level()` 在 `DialogSleeping` 状态下直接返回，不更新监听灯条。录音结束时也不再无条件调用 `set_light_strip_listening()`，而是重新应用当前状态输出。

这样可避免休眠期间虽然屏幕为黑色、灯带却随麦克风音量亮起的问题。

### 5.4 无 STT 结果时保持休眠

一次录音可能没有得到有效 STT。旧实现会在这种情况下强制设置 `Listening`，从而退出休眠。

当前录音开始时保存 `recognition_return_state`。如果没有 STT：

- 原状态是 `DialogSleeping`，恢复 `DialogSleeping`；
- 其他状态，恢复 `Listening`。

这样既重新打开 supervisor 的麦克风采样门控，也不会错误点亮设备。

### 5.5 屏幕触摸

当前产品规则是“对话休眠只响应唤醒词”。因此屏幕触摸在 `DialogSleeping` 状态下被忽略，不调用 `mark_user_interaction()`，也不切回 `Listening`。

长按停止属于安全/控制行为，仍按原逻辑执行，不受该规则限制。

## 6. 服务端状态机

服务端实现位于 `stack-chan/stack-chan-server/src/realtime_server.py`。

### 6.1 活跃状态

`RealtimeDeviceSession.dialog_awake` 表示当前会话是否允许正常对话：

```text
True  -> 正常处理休眠词、唤醒词和普通识别结果
False -> 只处理唤醒词，其他结果全部忽略
```

### 6.2 正确的判断顺序

服务端现在首先检查是否已经休眠：

```python
if not session.dialog_awake:
    if has_realtime_wake_word(text):
        # 唤醒并通知设备
    else:
        # 忽略所有非唤醒内容
        return
```

只有处于 awake 状态时，才继续判断休眠词并进入休眠。

这个顺序保证休眠期间以下内容都被忽略：

- 普通问题；
- 普通命令；
- 再次说“拜拜”；
- 其他休眠词；
- 不包含唤醒词的误识别文本。

被忽略时服务端记录 `dialog_sleeping_ignore`，并再次发送 `device_state=sleep`，用于校正设备显示状态。

### 6.3 进入休眠

awake 状态下识别到休眠词后，服务端按以下顺序执行：

1. 根据文本选择缓存告别语；
2. 发送 LLM 文本并入队 `speak` 命令；
3. 将 `dialog_awake` 设为 `False`；
4. 按设备保存休眠状态；
5. 记录 `dialog_sleep`；
6. 发送 `device_state=sleep`。

虽然告别语命令和状态消息来自不同传输路径，但设备端的 `DialogSleeping -> Speaking -> DialogSleeping` 返回逻辑能保证无论先收到哪一个，播放完成后都保持休眠。

### 6.4 唤醒

休眠状态下识别到唤醒词后，服务端：

1. 将 `dialog_awake` 设为 `True`；
2. 保存该设备的 awake 状态；
3. 发送 `device_state=listening`；
4. 如果识别文本只有唤醒词，播放随机唤醒回复并结束本轮；
5. 如果唤醒词后还包含用户问题，则继续把本轮文本提交到正常对话流程。

屏幕是在设备收到 `device_state=listening` 后才恢复默认表情，不是在 VAD 命中时恢复。

## 7. WebSocket 重连

### 7.1 服务端保持设备级状态

`RealtimeManager` 使用：

```python
self._dialog_awake_by_device: dict[str, bool] = {}
```

在 manager 生命周期内保存每个 `device_id` 的对话状态。

新 WebSocket 会话建立时不再无条件设置 `dialog_awake=True`，而是读取该设备上一次保存的状态，并立即发送：

```text
记忆状态为 awake    -> device_state=listening
记忆状态为 sleeping -> device_state=sleep
```

设备通过 `Device-Id` 请求头携带 MAC 地址，因此重连时服务端可在收到第一条设备消息之前恢复正确状态。设备 hello 更新 ID 后，服务端还会再次同步当前状态。

### 7.2 设备端恢复状态

设备发起重连前检查是否处于 `DialogSleeping`。连接成功后：

- 重连前为 `DialogSleeping`，先恢复 `DialogSleeping`；
- 否则恢复 `Listening`；
- 随后以服务端发来的 `device_state` 为最终状态。

这样可以避免重连窗口内短暂显示 `calm`。

### 7.3 保持范围

当前 `_dialog_awake_by_device` 是进程内内存状态：

- WebSocket 重连：保持；
- 设备会话替换：保持；
- 服务端进程重启：不保持，默认恢复 awake；
- 设备固件重启：本地状态不持久化，最终由服务端初始状态消息决定。

如果未来要求服务端重启后仍保持休眠，需要把该状态写入 SQLite 或其他持久化存储，并定义过期策略。

## 8. 端到端时序

### 8.1 正常进入休眠

```text
用户                 设备                    实时服务端             命令通道
 |  “拜拜”             |                         |                     |
 |-------------------->| VAD/录音/Opus           |                     |
 |                     |------------------------>| ASR final: 拜拜     |
 |                     |                         | 入队告别语 ---------->|
 |                     |<------------------------| device_state=sleep  |
 |                     | DialogSleeping          | dialog_awake=False  |
 |                     |<---------------------------------- speak       |
 |                     | Speaking: 播放“拜拜”     |                     |
 |                     | DialogSleeping          |                     |
 |                     | sleep_dark + 灯带关闭    |                     |
```

### 8.2 休眠期间普通语音

```text
用户                 设备                    实时服务端
 | “今天天气怎么样”     |                         |
 |-------------------->| 保持黑屏，录音上传       |
 |                     |------------------------>| ASR final
 |                     |                         | dialog_sleeping_ignore
 |                     |<------------------------| device_state=sleep
 |                     | 继续 DialogSleeping     |
```

### 8.3 唤醒

```text
用户                 设备                    实时服务端
 | “小派”              |                         |
 |-------------------->| 保持黑屏，录音上传       |
 |                     |------------------------>| ASR final: 小派
 |                     |                         | dialog_awake=True
 |                     |<------------------------| device_state=listening
 |                     | Listening + calm        |
 |                     |<------------------------| 唤醒回复
 |                     | Speaking -> Listening   |
```

## 9. 日志与诊断

### 9.1 服务端关键标记

| 标记 | 含义 |
| --- | --- |
| `sleep_reply_start` | 已识别休眠词，开始安排告别语 |
| `dialog_sleep` | 服务端已把对话状态切为休眠 |
| `dialog_sleeping_ignore` | 休眠期间识别到非唤醒内容并忽略 |
| `wake_reply_start` | 识别到纯唤醒词，开始安排唤醒回复 |

### 9.2 设备心跳预期

进入休眠并播放完告别语后，心跳应稳定显示：

```text
mode=dialog_sleeping face=sleep_dark
```

告别语播放期间可能短暂显示：

```text
mode=speaking face=sleep_dark
```

随后必须返回：

```text
mode=dialog_sleeping face=sleep_dark
```

如果出现 `mode=listening face=calm`，说明仍有某条路径错误地把设备切回了 `Listening`。

### 9.3 建议的人工验收步骤

1. 正常唤醒并进行一句普通对话。
2. 说“拜拜”，确认只播放一次告别语。
3. 等待告别语结束，确认屏幕和灯带保持关闭。
4. 再说“拜拜”，确认无回复、无亮屏。
5. 说一个普通问题，确认无回复、无亮屏。
6. 制造一次 WebSocket 断开重连，确认仍保持黑屏。
7. 说“小派”，确认设备点亮并播放唤醒回复。
8. 说“小派，今天天气怎么样”，确认同一轮既唤醒又继续处理问题。

## 10. 自动化验证

服务端测试位于：

```text
stack-chan/stack-chan-server/tests/test_realtime_mapping.py
```

新增覆盖包括：

- WebSocket 重连保留休眠状态；
- 第一次休眠词只播放一次告别语；
- 休眠期间普通文本被忽略；
- 休眠期间重复休眠词被忽略；
- 唤醒词可以从休眠状态恢复。

服务端全量测试命令：

```bash
cd /home/myendless/xiaopai/stack-chan/stack-chan-server
python3 -m unittest discover -s tests
```

当前验证结果为 97 个测试通过。

固件使用现有 Ninja 构建目录验证：

```bash
cd /home/myendless/xiaopai/stack-chan
/home/myendless/stack-chan/.espressif/tools/ninja/1.12.1/ninja -C build -j2
```

构建成功后生成：

```text
/home/myendless/xiaopai/stack-chan/build/xiaopai-counter-demo.bin
```

## 11. 当前限制与后续方向

### 11.1 不是低功耗休眠

`DialogSleeping` 仍保持麦克风采样、Wi-Fi、WebSocket、Opus 编码和云端 ASR，因此不会显著降低系统功耗或 ASR 成本。

### 11.2 依赖网络与服务端

由于唤醒词由服务端识别：

- 网络断开期间无法完成语音唤醒；
- ASR 服务不可用时无法识别唤醒词；
- 网络恢复后可以继续监听，但仍依赖重连成功。

### 11.3 服务端重启默认醒来

设备级 awake/sleeping 状态目前只保存在 `RealtimeManager` 内存中。服务端进程重启后，新设备会话默认 awake。

### 11.4 本地唤醒的演进方向

如果未来实现真正的 Quiet/低功耗模式，建议将当前方案演进为：

1. `DialogSleeping` 下不启动完整云端 ASR；
2. 设备本地运行关键词检测；
3. 命中“小派”等唤醒词后切换 `Listening`；
4. 再启动完整录音、WebSocket ASR 和正常对话；
5. Deep Sleep 作为另一种独立状态，不与 `DialogSleeping` 或 Quiet 混用。

## 12. 主要代码位置

| 文件 | 责任 |
| --- | --- |
| `stack-chan/main/voice_state.h` | 定义 `DialogSleeping` 和状态输出钩子 |
| `stack-chan/main/voice_state.cpp` | 状态名称、麦克风权限、播放返回状态和输出分派 |
| `stack-chan/main/xiaopai_state.cpp` | supervisor/interaction 映射和字符串状态解析 |
| `stack-chan/main/main.cpp` | 注册 `DialogSleeping` 输出钩子 |
| `stack-chan/main/main_app_state.inc` | 区分空闲黑屏与对话休眠 |
| `stack-chan/main/main_camera_motion.inc` | 休眠显示与灯带输出 |
| `stack-chan/main/main_tts_commands.inc` | 休眠时禁止监听灯效更新 |
| `stack-chan/main/main_realtime_speech.inc` | VAD、录音、无 STT 恢复和设备端重连 |
| `stack-chan/main/main_realtime_transport.inc` | WebSocket `device_state` 映射 |
| `stack-chan/main/main_command_services.inc` | HTTP/轮询状态命令映射 |
| `stack-chan/main/main_head_touch.inc` | 休眠期间触摸处理 |
| `stack-chan/stack-chan-server/src/realtime_server.py` | 服务端休眠过滤、唤醒和重连状态保持 |
| `stack-chan/stack-chan-server/tests/test_realtime_mapping.py` | 服务端状态机回归测试 |

