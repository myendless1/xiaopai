# Xiaopai / Stack-chan 完整系统架构重设计方案

> 目标：只参考当前仓库中已经实现或已经暴露的硬件资源、端口、引脚、配置项和服务接口，重新设计一套更清晰、更稳定、更适合 Stack-chan 本体的系统架构。本文不沿用当前代码中 demo/task 拼接式的系统架构。

---

## 1. 设计目标

小派需要在 Stack-chan / M5Stack CoreS3 本体上同时具备以下能力：

1. 长期聆听。
2. 触发式录音。
3. 录音状态下实时流式上传音频块做 ASR。
4. server 可下发说话、表情、移动、拍照、追踪、休眠、唤醒等命令。
5. 说话状态不可被打断。
6. 录音状态不可被说话打断。
7. 活跃和休眠两个宏状态。
8. 休眠状态下仍可聆听和录音，但不主动说话。
9. 休眠状态下识别到唤醒词后进入活跃状态，并立即播放预设回复。
10. DJI USB receiver 优先作为麦克风输入，未插入或不可采集时回退到内部麦克风。
11. USB 插入和拔出事件作为输入源切换标志。
12. 支持 DJI UAC 初始化、数据采集、声道选择、重采样。
13. 支持表情状态管理。
14. 支持舵机移动指令。
15. 支持拍照和视觉追踪。
16. 支持后台每隔几分钟触发一次久坐检测。
17. 所有功能逻辑上同时在线，但硬件资源冲突时必须通过状态机和资源锁串行执行。

---

## 2. 当前仓库已经具备或暴露的功能资源

### 2.1 网络和 server

当前仓库使用本地 server 作为主要后端。默认 HTTP base URL 为：

```text
http://<server-ip>:8091
```

固件中存在多个 server 候选地址，例如：

```text
http://1.14.134.217:8091/
http://192.168.21.15:8091
http://172.24.77.83:8091
http://192.168.137.1:8091
```

server 已经覆盖以下接口形态：

```http
GET  /health
GET  /devices
POST /command
GET  /device/next-command?device_id=<id>&timeout=<s>
GET  /device/ack?device_id=<id>&cmd_id=<id>&status=<status>
POST /device/event
POST /upload-audio
POST /upload-image
GET  /stream-speak?text=<text>
GET  /event-audio/<name>.pcm
```

实时语音 WebSocket 默认为：

```text
ws://<server-ip>:8092/xiaozhi/ws
```

### 2.2 音频和录音

当前仓库中主要音频参数如下：

| 参数 | 当前值 |
|---|---:|
| 协议采样率 | 16000 Hz |
| 录音 chunk | 40 ms |
| Opus frame | 60 ms |
| pre-roll | 800 ms |
| voice start threshold | 5000 |
| voice stop threshold | 1800 |
| max record | 15000 ms |
| silence stop | 2500 ms |

录音逻辑应保留这些资源参数，但重新组织状态和事件流。

### 2.3 内部麦克风

内部麦克风基于 CoreS3 的 ES7210，主要资源：

| 资源 | 当前值 |
|---|---:|
| I2S port | I2S_NUM_0 |
| MCLK | GPIO0 |
| WS | GPIO33 |
| BCLK | GPIO34 |
| DIN | GPIO14 |
| DOUT | GPIO13 |
| internal I2C port | I2C_NUM_1 |
| I2C SDA | GPIO12 |
| I2C SCL | GPIO11 |

### 2.4 DJI USB receiver

当前 DJI USB receiver 作为 UAC 麦克风输入，识别信息：

| 项 | 当前值 |
|---|---:|
| VID | 0x2ca3 |
| PID | 0x4011 |
| 输入采样率 | 48000 Hz |
| bit depth | 24-bit |
| channels | 2 |
| 输出给主链路 | 16000 Hz mono int16 |

DJI 输入已有的关键能力：

1. 注册 USB state callback。
2. 配置 UAC stream。
3. 通过 CoreS3 VBUS 重新枚举设备。
4. 接收 raw UAC frame。
5. 解析 8/16/24-bit little-endian PCM。
6. 自动选择左右声道中能量更高的通道。
7. 重采样到 16 kHz mono。
8. 输出到 PCM ringbuffer。
9. 插入时等待首帧 UAC PCM 后设置 capture ready。
10. 拔出时清空 capture ready，并丢弃 raw/PCM ringbuffer。

### 2.5 扬声器和 TTS

server 通过：

```http
GET /stream-speak?text=<text>
```

返回：

```text
Content-Type: application/octet-stream
X-Audio-Format: pcm_s16le
X-Sample-Rate: 16000
X-Channels: 1
```

固件端负责拉取 PCM 流并播放。

### 2.6 摄像头

CoreS3 摄像头当前使用 GC0308，QVGA，RGB565。

| 资源 | 当前值 |
|---|---:|
| frame size | 320 × 240 |
| format | RGB565 |
| d7 | GPIO47 |
| d6 | GPIO48 |
| d5 | GPIO16 |
| d4 | GPIO15 |
| d3 | GPIO42 |
| d2 | GPIO41 |
| d1 | GPIO40 |
| d0 | GPIO39 |
| VSYNC | GPIO46 |
| HREF | GPIO38 |
| PCLK | GPIO45 |
| I2C port | I2C_NUM_1 |

注意：摄像头会占用 internal I2C。拍照时必须避免同时刷新灯带或调用其他依赖 internal I2C 的操作。

### 2.7 舵机运动

当前头部 pan/tilt 通过 SCS 舵机控制。

| 资源 | 当前值 |
|---|---:|
| UART | UART_NUM_1 |
| TX | GPIO6 |
| RX | GPIO7 |
| baud | 1000000 |
| pan servo id | 1 |
| tilt servo id | 2 |
| yaw zero raw | 460 |
| pitch zero raw | 620 |
| steps per degree | 3.2 |
| yaw range | -180° 到 180° |
| pitch range | 0° 到 90° |

### 2.8 表情和灯带

表情支持：

```text
calm
sleep_dark
screen_off
shy
thinking
relaxed
smile
smile_blink
happy
happy_dynamic
wink
wink_blink
grin
grin_blink
```

灯带资源：

| 资源 | 当前值 |
|---|---:|
| PY32 I2C address | 0x6f |
| RGB pin | 13 |
| LED count | 12 |
| side LED count | 6 |

表情和灯带要统一由 `ExpressionManager` 管理，其他模块不得直接操作显示和灯带。

### 2.9 久坐检测

当前代码中已有久坐检测参数：

| 参数 | 当前值 |
|---|---:|
| 检测间隔 | 5 分钟 |
| 连续 owner hit 次数 | 5 |

提醒语料包括：

```text
你已连续工作好长时间啦，起身拉伸一下吧。
小派观察到你一直在忙，站起来活动两分钟吧。
眼睛和肩颈都需要休息一下，起身走一走吧。
```

---

## 3. 总体架构

### 3.1 核心设计原则

Stack-chan/CoreS3 本体资源有限，所以“所有功能同时运行”不能理解成所有硬件同时抢资源。

合理定义应是：

```text
音频输入链路常驻运行。
状态机常驻运行。
server 命令接收常驻运行。
久坐检测定时器常驻运行。
视觉、运动、说话、表情作为事件在线等待。
真正使用同一硬件资源时通过资源锁串行执行。
```

### 3.2 总体模块图

```text
                 ┌──────────────────────┐
                 │     Xiaopai Server     │
                 │   HTTP 8091 + WS 8092  │
                 └──────────┬───────────┘
                            │
        ┌───────────────────┼───────────────────┐
        │                   │                   │
   STT stream          Command channel       Image/TTS
   WebSocket           long-poll / WS        HTTP
        │                   │                   │
┌───────▼───────────────────▼───────────────────▼───────┐
│                    Firmware EventBus                    │
│       priority queue + delayed queue + coalescing        │
└───────────┬─────────────┬──────────────┬───────────────┘
            │             │              │
┌───────────▼───┐ ┌───────▼──────┐ ┌─────▼────────┐
│ StateManager  │ │ AudioManager │ │ CommandMgr   │
│ macro+voice   │ │ mic+vad+rec  │ │ admission    │
└───────┬───────┘ └───────┬──────┘ └─────┬────────┘
        │                 │              │
┌───────▼───────┐ ┌───────▼──────┐ ┌─────▼────────┐
│ SpeechManager │ │ VisionMgr    │ │ MotionCtrl   │
│ TTS playback  │ │ camera+face  │ │ pan/tilt     │
└───────┬───────┘ └───────┬──────┘ └─────┬────────┘
        │                 │              │
┌───────▼─────────────────▼──────────────▼───────┐
│ ExpressionManager + SedentaryService + ResourceLock │
└─────────────────────────────────────────────────┘
```

### 3.3 模块职责

| 模块 | 职责 |
|---|---|
| `StateManager` | 唯一维护活跃/休眠状态和聆听/录音/说话状态 |
| `EventBus` | 全局事件队列，处理优先级、合并、延迟、过期 |
| `AudioInputManager` | 管理 DJI USB receiver 和内部麦克风，输出统一 16 kHz mono PCM |
| `VadRecorder` | 声音阈值检测、pre-roll、录音开始/停止、尾部延迟 |
| `SttClient` | 实时上传音频块，接收 partial/final STT |
| `SpeechManager` | 拉取 TTS PCM，播放语音，保证说话不可打断 |
| `CommandManager` | 接收 server 命令，并根据状态决定执行、延后、拒绝、过期 |
| `VisionManager` | 拍照、图片上传、人脸检测、追踪、久坐检测 |
| `MotionController` | 舵机移动、回中、点头、追踪修正 |
| `ExpressionManager` | 屏幕表情、眨眼、灯带、sleep_dark、录音/说话动效 |
| `ResourceLockManager` | 管理 AUDIO_IN、AUDIO_OUT、USB_HOST、CAMERA_I2C、DISPLAY、SERVO_UART |

---

## 4. 状态机设计

### 4.1 两层状态

系统采用二维状态：

```cpp
enum class MacroState {
    Active,
    Sleeping
};

enum class VoiceState {
    Listening,
    Recording,
    Speaking
};
```

不要再把 `Waiting` 或 `Thinking` 作为语音状态。`Thinking` 只属于 UI 或任务状态，不应该影响能不能采样麦克风。

### 4.2 状态组合含义

| MacroState | VoiceState | 行为 |
|---|---|---|
| Active | Listening | 正常聆听，可被 server speak 打断 |
| Active | Recording | 正在录音，实时上传音频块，不允许 speak 打断 |
| Active | Speaking | 正在说话，不允许普通事件打断 |
| Sleeping | Listening | 屏幕 sleep_dark，但麦克风仍监听 |
| Sleeping | Recording | 可录音并上传 STT，只处理唤醒词 |
| Sleeping | Speaking | 不作为稳定状态存在，必须先唤醒再说话 |

### 4.3 主转移图

```text
Active / Listening
  ├─ voice_level >= start_threshold → Active / Recording
  ├─ server_speak → Active / Speaking
  ├─ idle_timeout → Sleeping / Listening
  └─ server_sleep 或 STT sleep keyword → Sleeping / Listening

Active / Recording
  ├─ silence >= tail_ms → Active / Listening
  ├─ max_record_ms → Active / Listening
  ├─ STT final contains sleep keyword → Sleeping / Listening
  └─ server_speak → defer，不打断

Active / Speaking
  └─ speech_done → Active / Listening

Sleeping / Listening
  ├─ voice_level >= start_threshold → Sleeping / Recording
  ├─ server_speak + wake_for_speech=true → Active / Speaking
  └─ server_speak without wake_for_speech → reject 或 defer

Sleeping / Recording
  ├─ STT final contains wake word → Active / Speaking，播放唤醒回复
  ├─ STT final no wake word → Sleeping / Listening
  └─ server_speak → reject 或 defer
```

### 4.4 活跃和休眠规则

系统维护两个时间戳：

```cpp
uint64_t last_recording_finished_at;
uint64_t last_speech_finished_at;
```

进入休眠：

```cpp
if (now - max(last_recording_finished_at, last_speech_finished_at) > idle_sleep_ms) {
    macro_state = MacroState::Sleeping;
}

if (stt_final_text contains sleep_keyword) {
    macro_state = MacroState::Sleeping;
}
```

退出休眠：

```cpp
if (stt_final_text contains wake_word) {
    macro_state = MacroState::Active;
    enqueue_speak("我在。", wake_reply=true);
}

if (server_command.wake_for_speech) {
    macro_state = MacroState::Active;
    enqueue_speak(server_command.text);
}
```

### 4.5 唤醒词和休眠词

建议保留当前 server 中已有词表。

唤醒词示例：

```text
小派同学
小派同學
小派
小胖
小盼
小潘
小排
小白
小机器
机器人
xiaopai
```

休眠词示例：

```text
拜拜
再见
退下
退下吧
休息
睡觉
睡眠
先这样
```

---

## 5. EventBus 事件系统

### 5.1 事件类型

```cpp
enum class EventType {
    UsbAttached,
    UsbDetached,
    DjiFirstPcmReady,
    AudioLevelStart,
    AudioLevelStop,
    RecordingFinalText,
    RecordingTimeout,
    ServerSpeak,
    ServerMove,
    ServerFace,
    ServerCapture,
    ServerFindOwner,
    ServerSleep,
    ServerWake,
    SpeechDone,
    SedentaryTick,
    CameraResult,
    MotionDone,
    Fault
};
```

### 5.2 队列分层

```text
RealtimeEventQueue
  P0/P1：USB、录音开始/结束、说话完成、状态切换

CommandEventQueue
  P2/P3：server speak/move/face/camera/find_owner/sequence

BackgroundJobQueue
  P4：久坐检测、健康检查、预热 TTS cache、内存日志
```

音频 PCM 不走普通事件队列，而走 ringbuffer：

```text
internal mic / DJI mic
  → 16 kHz mono PCM
  → clean_audio_ringbuffer
  → VAD
  → Recorder
  → STT stream
```

### 5.3 事件准入矩阵

| 当前状态 | ServerSpeak | ServerMove | Capture/FindOwner | Face | USB 插拔 | SedentaryTick |
|---|---|---|---|---|---|---|
| Active/Listening | 立即执行 | 可执行 | 可执行 | 可执行 | 立即处理 | 可执行 |
| Active/Recording | 延后 | 延后 | 延后 | 可低优先级显示 | 立即处理，但不拼接音频 | 延后 |
| Active/Speaking | 延后 | 延后 | 延后 | 只允许 speaking 内部表情 | 记录状态，不打断 | 延后 |
| Sleeping/Listening | 仅 `wake_for_speech` 执行 | 延后/拒绝 | 可执行低频检测 | 保持 sleep_dark | 立即处理 | 可执行 |
| Sleeping/Recording | 延后/拒绝 | 延后 | 延后 | 不显示普通表情 | 立即处理 | 延后 |

### 5.4 推荐优先级

```text
P0  HardwareFault, UsbDetached, AudioSourceLost
P1  AudioLevelStart, AudioLevelStop, SpeechDone, STTFinal
P2  ServerWake, ServerSleep, ServerSpeak(wake_for_speech)
P3  ServerSpeak, ServerMove, ServerFindOwner, ServerCapture
P4  ServerFace, volume, debug
P5  SedentaryTick, memory monitor, prewarm cache
```

注意：高优先级不代表可以打断 `Speaking`。`Speaking` 是不可打断临界区。

---

## 6. 资源锁设计

### 6.1 资源类型

```cpp
enum class ResourceLock {
    AudioInput,
    AudioOutput,
    UsbHost,
    CameraI2c,
    Display,
    LightStrip,
    ServoUart,
    NetworkHttp,
    NetworkWs
};
```

### 6.2 资源冲突表

| 资源 | 冲突对象 | 原因 |
|---|---|---|
| `AudioInput` | 录音、源切换 | 避免一句录音中混用两个输入源 |
| `AudioOutput` | TTS、tone、play_audio | 说话不可打断 |
| `UsbHost` | DJI UAC bring-up | USB 枚举和数据采集需要独占状态 |
| `CameraI2c` | 摄像头、灯带、M5 update | camera 会占用 internal I2C |
| `Display` | 表情、状态页、调试页 | 避免屏幕并发绘制 |
| `LightStrip` | 表情灯效、录音电平灯效 | 灯带状态应统一管理 |
| `ServoUart` | move、track、nod、find_owner | 避免多个运动命令交错 |
| `NetworkHttp` | TTS、image upload、command poll | ESP32 网络资源有限，需要限流 |
| `NetworkWs` | STT stream | 语音实时链路优先 |

### 6.3 camera/I2C 特别规则

当 `CameraI2c` 被占用时：

1. 不刷新灯带。
2. 不做 `M5.update()`。
3. 表情刷新可以记录为 pending，但不立即写屏。
4. 拍照完成后恢复表情状态。

---

## 7. 麦克风输入源切换设计

### 7.1 输入源状态

```cpp
enum class MicSource {
    InternalMic,
    DjiUsb
};

struct MicSourceState {
    bool dji_detected;
    bool dji_capture_ready;
    bool dji_identity_confirmed;
    bool internal_ready;
    MicSource active;
    MicSource pending;
    uint64_t last_switch_at;
};
```

### 7.2 选择策略

```cpp
MicSource preferred_source() {
    if (dji_capture_ready && dji_identity_confirmed) {
        return MicSource::DjiUsb;
    }
    return MicSource::InternalMic;
}
```

### 7.3 切换规则

1. `Listening` 状态允许切换。
2. DJI ready 后立即切到 DJI。
3. DJI 断开后切回 internal mic。
4. `Recording` 中不做普通源切换。
5. `Recording` 中如果当前源断开，则本次录音标记为 `source_lost`，发送 `listen stop`，等待已有 STT，然后回到 Listening。
6. `Speaking` 中只更新 pending source，播放结束后再切换。
7. 插拔事件需要 debounce，建议 500 到 1000 ms。

### 7.4 DJI UAC setup 流程

```text
boot
  → create raw_ringbuf
  → create pcm_ringbuf
  → register usb_state_cb
  → uac_streaming_config(48k, 24bit, 2ch)
  → create decode_task
  → VBUS off 800ms
  → usb_streaming_start()
  → wait 200ms
  → VBUS on
  → wait 1200ms
  → wait STREAM_CONNECTED
  → wait first UAC frame
  → set capture_ready
  → emit DjiFirstPcmReady
```

### 7.5 DJI 数据转换链路

```text
UAC frame 48k / 24bit / 2ch
  → parse little-endian sample
  → compute channel energy
  → select best channel
  → mono
  → downsample to 16k
  → int16 PCM
  → clean_audio_ringbuffer
```

---

## 8. 聆听、录音、ASR 设计

### 8.1 Listening 状态

Listening 状态只做阈值检测和 pre-roll，不上传所有背景音。

```cpp
while (VoiceState == Listening) {
    pcm = AudioInputManager.read_16k(40ms);
    level = avg_abs(pcm);
    preroll_ring.push(pcm);

    if (level >= start_threshold) {
        emit AudioLevelStart(preroll_ring.snapshot());
    }
}
```

### 8.2 Recording 状态

进入 Recording 后开始一次 utterance。

```cpp
on AudioLevelStart:
    StateManager.set_voice(Recording);
    SttClient.start_utterance(source, sample_rate=16000);
    SttClient.send(preroll);
    silence_ms = 0;
```

录音循环：

```cpp
while (VoiceState == Recording) {
    pcm = AudioInputManager.read_16k(40ms);
    SttClient.send(pcm);

    level = smooth(avg_abs(pcm));
    if (level < stop_threshold) {
        silence_ms += 40;
    } else {
        silence_ms = 0;
    }

    if (silence_ms >= silence_tail_ms && elapsed_ms > min_record_ms) {
        break;
    }

    if (elapsed_ms >= max_record_ms) {
        break;
    }
}

SttClient.stop_utterance();
StateManager.set_voice(Listening);
```

### 8.3 STT final 处理

```cpp
on STT_FINAL(text):
    last_recording_finished_at = now;

    if (MacroState == Sleeping) {
        if (contains_wake_word(text)) {
            MacroState = Active;
            enqueue Speak("我在。", priority=P2, source=WakeReply);
        } else {
            StateManager.set_voice(Listening);
        }
        return;
    }

    if (MacroState == Active) {
        if (contains_sleep_word(text)) {
            MacroState = Sleeping;
            ExpressionManager.show_sleep_dark();
            StateManager.set_voice(Listening);
            return;
        }

        ServerPolicy.forward_user_text(text);
    }
```

---

## 9. 说话状态设计

### 9.1 说话不可打断

说话由 `SpeechManager` 独占 `AudioOutput`。

```cpp
on ServerSpeak when admissible:
    StateManager.set_voice(Speaking);
    ExpressionManager.on_speaking_start();
    TtsStreamClient.open("/stream-speak");
    AudioOutput.play_until_eof();
    emit SpeechDone;
```

退出说话：

```cpp
on SpeechDone:
    ExpressionManager.on_speaking_end();
    last_speech_finished_at = now;
    StateManager.set_voice(Listening);
```

### 9.2 Speak 命令处理规则

| 当前状态 | speak 处理 |
|---|---|
| Active/Listening | 立即进入 Speaking |
| Active/Recording | ACK `deferred_recording`，排队 |
| Active/Speaking | ACK `deferred_speaking`，排队或合并 |
| Sleeping/Listening，普通 speak | ACK `rejected_sleep_policy` 或 `deferred_sleep` |
| Sleeping/Listening，`wake_for_speech=true` | 先 Active，再 Speaking |
| Sleeping/Recording | 延后或拒绝 |

### 9.3 interrupt 语义重定义

不建议保留粗暴的：

```json
"interrupt": true
```

建议改为：

```json
"admission": {
  "interrupt_policy": "listening_only",
  "wake_for_speech": false,
  "allow_in_sleep": false
}
```

可选值：

```text
none
listening_only
after_recording
```

---

## 10. 表情状态管理

### 10.1 表情由状态派生

```text
Active / Listening   → calm/listening + green listening light
Active / Recording   → listening + audio level bar
Active / Speaking    → speaking animation + blue speaking light
Sleeping / Listening → sleep_dark + low brightness listening light
Sleeping / Recording → sleep_dark + minimal recording indicator
```

### 10.2 server face 命令作为 overlay

```cpp
struct ExpressionOverlay {
    std::string name;
    uint64_t expire_at;
    bool restore_after;
    int priority;
};
```

规则：

1. Speaking 内部的 speaking 表情优先级最高。
2. sleep_dark 不能被普通 face 命令覆盖。
3. temporary expression 到期后恢复由 `StateManager` 派生的默认表情。
4. camera 占用 internal I2C 时，只记录 pending light state，不立即刷新灯带。
5. 任何模块不得直接调用屏幕和灯带，只能向 `ExpressionManager` 发事件。

---

## 11. 移动和追踪设计

### 11.1 MotionCommand

```cpp
struct MotionCommand {
    enum class Type {
        Relative,
        Absolute,
        Home,
        Nod
    } type;

    float pan_deg;
    float tilt_deg;
    int duration_ms;
};
```

### 11.2 move 命令映射

| command | 行为 |
|---|---|
| left | pan 减小 degree |
| right | pan 增加 degree |
| up | tilt 增加 degree |
| down | tilt 减小 degree |
| center/home | pan=0，tilt=home_pitch |
| absolute | 使用 payload 中 pan/tilt |

### 11.3 追踪任务

追踪不要做成无限循环，而应做成一个可取消 job：

```text
FindOwnerJob
  → acquire CAMERA_I2C_LOCK
  → acquire SERVO_UART_LOCK
  → move scan pose
  → capture frame
  → upload image
  → parse face center
  → compute yaw/pitch correction
  → move servo
  → optional refine
  → release locks
```

如果当前处于 `Recording` 或 `Speaking`，追踪 job 必须延后。

### 11.4 人脸追踪计算

使用当前相机模型参数：

```text
cx = 160
cy = 120
fx = 364
fy = 364
```

计算：

```cpp
dx = face_center_x - cx;
dy = face_center_y - cy;

yaw_delta_deg = atan(dx / fx) * 180 / pi;
pitch_delta_deg = atan(dy / fy) * 180 / pi;

new_yaw = current_yaw + yaw_delta_deg * yaw_gain * yaw_direction;
new_pitch = current_pitch + pitch_delta_deg * pitch_gain * pitch_direction;
```

---

## 12. 拍照和图片上传设计

### 12.1 CaptureJob

```text
CaptureJob
  → check state admission
  → acquire CAMERA_I2C_LOCK
  → init camera if needed
  → discard stale frames
  → capture one RGB565 frame
  → release camera if policy says one-shot
  → POST /upload-image
  → parse response
  → emit CameraResult
```

### 12.2 upload-image 请求

```http
POST /upload-image
Content-Type: image/rgb565
X-Image-Format: rgb565
X-Image-Width: 320
X-Image-Height: 240
X-Device-Id: <device_id>
X-Client-Id: <client_id>
X-Visual-Tracking: true|false
X-Purpose: capture|track|sedentary
```

### 12.3 server 返回

```json
{
  "type": "image_result",
  "device_id": "44:1b:f6:xx:xx:xx",
  "face_detection": {
    "backend": "yunet",
    "best_face": {
      "center": {"x": 180.0, "y": 110.0},
      "confidence": 0.92,
      "area": 12345
    }
  },
  "sedentary": {
    "owner_present": true,
    "sitting": true,
    "confidence": 0.85
  }
}
```

---

## 13. 久坐检测设计

### 13.1 触发策略

久坐检测是低优先级后台任务。

```cpp
SedentaryService:
    every 5 minutes:
        if VoiceState != Listening:
            delay
        if high_priority_command_pending:
            delay
        if camera_busy:
            delay
        enqueue SedentaryCheckJob
```

### 13.2 检测流程

```text
SedentaryTick
  → Capture frame
  → POST /upload-image with X-Purpose: sedentary
  → server returns owner_present + sitting confidence
  → if owner_present and sitting:
        sedentary_hit_count += 1
    else:
        sedentary_hit_count = 0
  → if sedentary_hit_count >= N:
        enqueue Speak(reminder)
        sedentary_hit_count = 0
```

### 13.3 休眠状态下的久坐检测

| MacroState | 久坐检测策略 |
|---|---|
| Active | 正常检测和提醒 |
| Sleeping | 可低频检测，但默认不直接说话 |
| Speaking | 延后 |
| Recording | 延后 |

如果休眠状态下发现久坐，可以缓存提醒。等用户唤醒后再提示。除非 server 或策略显式设置：

```json
"wake_for_speech": true
```

---

## 14. server 重新设计

### 14.1 server 角色

server 不应该直接假设自己能打断本体状态。server 只负责：

1. ASR。
2. TTS。
3. 图片识别。
4. 命令排队。
5. 设备在线状态。
6. command ack 管理。
7. OpenClaw / agent 接入。
8. 预设语音 cache。

本体是否执行命令，由固件端 `CommandManager` 根据状态机做 admission。

### 14.2 HTTP 管理通道

```http
GET  /health
GET  /devices
POST /command
GET  /device/next-command?device_id=<id>&timeout=<s>
GET  /device/ack?device_id=<id>&cmd_id=<id>&status=<status>
POST /device/event
POST /upload-image
GET  /stream-speak?text=<text>
GET  /event-audio/<name>.pcm
```

### 14.3 实时语音 WebSocket

```text
ws://<server-ip>:8092/xiaozhi/ws
```

设备到 server：

```json
{"type": "hello", "device_id": "..."}
{"type": "listen", "state": "start", "source": "dji_usb", "sample_rate": 16000}
```

之后发送 binary Opus audio frames。

结束：

```json
{"type": "listen", "state": "stop"}
```

server 到设备：

```json
{"type": "hello", "session_id": "..."}
{"type": "stt", "text": "...", "is_final": false}
{"type": "stt", "text": "...", "is_final": true}
{"type": "device_state", "state": "listening"}
{"type": "command", "command": {...}}
```

### 14.4 新 command schema

```json
{
  "cmd_id": "cmd_xxx",
  "type": "speak",
  "priority": 50,
  "ttl_seconds": 30,
  "discardable": false,
  "coalesce_key": "speak",
  "admission": {
    "interrupt_policy": "listening_only",
    "wake_for_speech": false,
    "allow_in_sleep": false
  },
  "payload": {
    "text": "你好，我是小派。",
    "cache_name": "hello",
    "voice": "zhimiao_emo",
    "volume": 80,
    "speech_rate": 0,
    "pitch_rate": 0
  }
}
```

### 14.5 command 类型

| type | payload |
|---|---|
| `speak` | text、cache_name、voice、volume、speech_rate、pitch_rate |
| `face` | expression、duration_ms |
| `move` | type、degree、pan、tilt、duration_ms |
| `capture_image` | purpose、visual_tracking |
| `find_owner` | rounds、gain_x、gain_y、stop_pixels、reply |
| `sleep` | reason |
| `wake` | reply |
| `volume` | direction、step、value |
| `sequence` | steps array |
| `stop` | emergency only |

### 14.6 ACK 状态

建议扩展 ack 状态：

```text
received
queued
deferred_recording
deferred_speaking
deferred_sleep
running
done
failed
expired
rejected_state
rejected_sleep_policy
```

示例：

```http
GET /device/ack?device_id=<id>&cmd_id=<cmd_id>&status=deferred_recording&message=recording_active
```

---

## 15. 推荐固件任务划分

| Task | Core | 优先级 | 说明 |
|---|---:|---:|---|
| `audio_input_task` | 0 | 5 | 内部 mic / DJI mic 输入，写 clean PCM ring |
| `vad_recorder_task` | 1 | 5 | 阈值监听、录音、STT 上传 |
| `speech_output_task` | 1 | 5 | TTS 播放，不可打断 |
| `event_router_task` | 1 | 6 | 唯一状态机和事件准入 |
| `command_rx_task` | 0 | 4 | HTTP long-poll 或 WS command |
| `vision_worker_task` | 0 | 3 | camera/photo/find-owner/sedentary |
| `motion_worker_task` | 0 | 3 | 舵机动作 |
| `expression_task` | 1 | 2 | 表情、眨眼、灯带 |
| `health_task` | 0 | 1 | 内存、心跳、日志 |

### 15.1 不建议保留的模式

不建议保留下面这种模式：

```text
run_camera_upload_app()
run_tracking_user_demo()
run_xiaozhi_ota_probe()
run_stream_tts_demo()
```

作为主架构中的长期入口。

这些可以保留为 debug/test，但正式系统中应全部收敛为：

```text
Event → Admission → Worker → Result Event → ACK
```

---

## 16. 关键流程

### 16.1 启动流程

```text
app_main
  → init NVS
  → init M5
  → init StateManager
  → init EventBus
  → init ExpressionManager
  → init AudioInputManager
  → init SpeechManager
  → init CommandManager
  → start audio_input_task
  → start vad_recorder_task
  → start event_router_task
  → start command_rx_task
  → start expression_task
  → start health_task
  → start sedentary timer
  → enter Active / Listening
```

### 16.2 DJI 插入流程

```text
USB attached
  → emit UsbAttached
  → DjiUsbUacDriver waits first PCM
  → emit DjiFirstPcmReady
  → if VoiceState == Listening:
        switch active source to DjiUsb
    else:
        set pending source to DjiUsb
```

### 16.3 DJI 拔出流程

```text
USB detached
  → emit UsbDetached
  → DjiUsbUacDriver clears capture_ready and ringbuffer
  → if active source is DjiUsb:
        if VoiceState == Recording:
            mark current utterance source_lost
            SttClient.stop_utterance()
            VoiceState = Listening
        switch source to InternalMic
```

### 16.4 server speak 流程

```text
server POST /command speak
  → server queue
  → firmware poll /device/next-command
  → CommandManager receives ServerSpeak
  → EventRouter admission
  → if Active/Listening:
        VoiceState = Speaking
        GET /stream-speak
        play PCM
        SpeechDone
        VoiceState = Listening
    else if Recording:
        ACK deferred_recording
    else if Speaking:
        ACK deferred_speaking
    else if Sleeping and wake_for_speech=false:
        ACK rejected_sleep_policy
    else if Sleeping and wake_for_speech=true:
        MacroState = Active
        VoiceState = Speaking
```

### 16.5 休眠唤醒流程

```text
Sleeping / Listening
  → sound threshold triggered
  → Sleeping / Recording
  → stream audio to server ASR
  → STT final returned
  → if contains wake word:
        MacroState = Active
        enqueue Speak("我在。")
        Active / Speaking
        SpeechDone
        Active / Listening
    else:
        Sleeping / Listening
```

### 16.6 久坐检测流程

```text
Sedentary timer every 5 min
  → EventBus enqueue SedentaryTick
  → EventRouter checks state
  → if VoiceState == Listening and no high priority command:
        VisionManager.capture
        POST /upload-image purpose=sedentary
        parse owner_present / sitting
        update hit count
        if hit count >= threshold:
            enqueue Speak(sedentary reminder)
  → else:
        delay SedentaryTick
```

---

## 17. 推荐文件结构

建议重构后的固件目录：

```text
stack-chan/main/
  app_main.cpp

  core/
    xiaopai_state_machine.h
    xiaopai_state_machine.cpp
    event_bus.h
    event_bus.cpp
    resource_lock.h
    resource_lock.cpp
    command_admission.h
    command_admission.cpp

  audio/
    audio_input_manager.h
    audio_input_manager.cpp
    internal_mic_input.h
    internal_mic_input.cpp
    dji_usb_uac_input.h
    dji_usb_uac_input.cpp
    vad_recorder.h
    vad_recorder.cpp
    stt_stream_client.h
    stt_stream_client.cpp
    speech_manager.h
    speech_manager.cpp

  vision/
    camera_manager.h
    camera_manager.cpp
    vision_client.h
    vision_client.cpp
    sedentary_service.h
    sedentary_service.cpp

  motion/
    motion_controller.h
    motion_controller.cpp
    tracking_controller.h
    tracking_controller.cpp

  ui/
    expression_manager.h
    expression_manager.cpp
    light_strip.h
    light_strip.cpp

  net/
    server_client.h
    server_client.cpp
    command_channel.h
    command_channel.cpp
    tts_client.h
    tts_client.cpp
```

server 目录建议：

```text
stack-chan/stack-chan-server/src/
  server.py
  command_queue.py
  device_registry.py
  realtime_asr_server.py
  tts_service.py
  image_service.py
  policy.py
  openclaw_bridge.py
  schemas.py
```

---

## 18. 落地顺序

### 第一阶段：状态机和事件总线

1. 新建 `xiaopai_state_machine`。
2. 新建 `event_bus`。
3. 统一状态变更入口。
4. 禁止其他模块直接设置 voice state。
5. 把 `Waiting/Thinking` 从语音状态中移除。

### 第二阶段：音频输入重构

1. 保留现有 ES7210 初始化代码。
2. 保留现有 DJI UAC driver 的底层能力。
3. 新建 `AudioInputManager` 包装两种输入源。
4. SourceSelector 只在安全点切换。
5. Recording 中禁止普通源切换。

### 第三阶段：录音和 STT 重构

1. 新建 `VadRecorder`。
2. 新建 `SttClient`。
3. 明确 utterance 生命周期。
4. 实现 server speak 在 Recording 中排队而不是打断。

### 第四阶段：command admission

1. server 命令进入 `CommandEventQueue`。
2. `EventRouter` 根据状态机准入。
3. 扩展 ACK 状态。
4. 统一支持 deferred、expired、rejected。

### 第五阶段：视觉和久坐检测

1. 新建 `VisionManager`。
2. 拍照、find_owner、sedentary 都走同一个 worker。
3. camera/I2C 加资源锁。
4. 久坐检测低优先级调度。

### 第六阶段：server schema 收敛

1. 保留 8091 HTTP 和 8092 WS。
2. 统一 command schema。
3. 添加 admission 字段。
4. 添加设备状态上报。
5. 让 server 不再假设自己能强制打断设备。

---

## 19. 最终架构结论

推荐的小派正式架构可以概括为：

```text
音频输入常驻
+ 三态语音状态机
+ 活跃/休眠宏状态机
+ 中央事件总线
+ 资源锁
+ server 命令准入策略
```

这样可以实现：

1. DJI USB receiver 和内部麦克风稳定切换。
2. 聆听、录音、说话三种状态互不混乱。
3. 录音状态不被说话打断。
4. 说话状态不可被打断。
5. 休眠状态仍可监听和录音。
6. 唤醒词可以唤醒并触发预设回复。
7. server 命令不会破坏本体状态。
8. 表情、灯带、舵机、摄像头都由资源锁保护。
9. 久坐检测可以后台长期存在，但不会抢占音频和说话。
10. 整套系统适合在 Stack-chan 这种资源紧张的机器人本体上长期稳定运行。
