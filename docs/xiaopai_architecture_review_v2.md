# Xiaopai / Stack-chan 架构审视与 V2 可落地方案

## 0. 审视范围

本报告只基于用户上传的 `repo.zip` 和原始架构设计稿。

已完成以下检查：

1. 阅读固件主逻辑、音频、USB UAC、摄像头、舵机、表情、命令和 OTA 代码。
2. 阅读本地 server 的 HTTP 命令队列、实时语音、TTS、ASR 和视觉代码。
3. 核对 CoreS3 内部 I2C、I2S、USB Host、UART、PSRAM 和任务配置。
4. 检查已有构建结果。固件已成功生成约 1.7 MB 的 app 镜像，能够放入当前 7 MB OTA 分区。
5. 运行 server 测试。结果为 66 项全部通过。

因此，当前问题不是“完全无法编译”。主要问题是长期运行时的并发一致性、资源恢复、硬件边界和产品语义。

---

## 1. 总体结论

当前仓库已经具备可工作的功能原型。主要链路都有实现：

- 内置 ES7210 麦克风。
- DJI USB UAC 麦克风。
- 16 kHz PCM 和 Opus。
- 实时 ASR 和 TTS。
- GC0308 拍照。
- YuNet 人脸检测。
- SCS 舵机控制。
- 表情和灯带。
- HTTP 命令队列。
- OTA。

但当前系统还不适合作为“长期稳定在线机器人固件”。存在 8 个优先级很高的问题：

1. 状态由多个任务共同修改。`volatile` 不能提供跨核同步。
2. 录音状态没有进入正式状态机，只由额外布尔变量维护。
3. HTTP 收命令的任务会同步执行拍照、追踪和 sequence。长任务会阻塞后续命令接收。
4. speak 队列长度只有 1。新 speak 会先打断当前语音，再覆盖旧命令。
5. DJI 和内置麦克风可能在一句录音中途切换，音频帧没有 source generation。
6. 当前休眠本质上只是黑屏。仍依赖联网 ASR 判断唤醒词，不是真正休眠。
7. 摄像头和内部 I2C 的冲突模型不准确。当前代码既过度阻塞 `M5.update()`，又没有真正保护所有 I2C 调用。
8. 久坐检测只判断连续检测到人脸，不能判断用户是否坐着。

推荐的 V2 核心不是“更复杂的通用 EventBus”。推荐使用：

```text
单一 Supervisor 状态所有者
+ 固定大小消息队列
+ 每个硬件只有一个 owner task
+ 音频源租约
+ 命令 lease、ACK、重试和去重
+ 安全优先的可取消作业
```

---

## 2. 关键问题清单

## 2.1 P0：状态机不是单一事实来源

当前 `voice_state.cpp` 用以下变量维护状态：

```text
main/voice_state.cpp:10-13
state
return_state
speaking_depth
generation
```

它们都是 `volatile`，没有 mutex、critical section 或单一任务所有权。

仓库中还存在另一组并行状态：

```text
main/main_app_state.inc:157-188
voice_listener_paused
voice_recording_active
speech_playback_active
realtime_tts_playback_active
speak_command_active
camera_owns_internal_i2c
...
```

这会产生无法被类型系统阻止的组合。例如：

```text
LocalVoiceState == Listening
voice_recording_active == true
speech_playback_active == true
```

当前代码用 generation 和多个 guard 尝试避免冲突，但它不能保证跨核原子转移。

### 修改建议

由 `supervisor_task` 独占状态。其他任务不能直接写状态。

所有状态变化必须发消息：

```cpp
SupervisorEvent {
    EventType type;
    uint32_t request_id;
    uint32_t generation;
    ...
};
```

状态建议保留两层，但不要删除 Waiting：

```cpp
enum class PresenceMode {
    Active,
    Quiet,
    Fault
};

enum class InteractionState {
    Monitoring,
    Recording,
    WaitingReply,
    Speaking
};
```

`WaitingReply` 是真实运行状态，不只是 UI。它决定是否允许立即开始下一次录音，以及新命令是否可以执行。

`Thinking` 可以是 UI 名称，但 `WaitingReply` 不能从调度状态中消失。

---

## 2.2 P0：新 speak 会打断当前 speak，并覆盖未执行命令

当前代码的 speak 队列只有 1 个元素：

```text
main/main_tts_commands.inc:557-561
xQueueCreate(1, sizeof(SpeakCommandItem))
```

入队前会执行：

```text
main/main_tts_commands.inc:511-529
request_speak_preempt("new speak command")
xQueueOverwrite(...)
```

因此当前行为是：

1. 新 speak 会中断正在播放的 speak。
2. 队列中已有 speak 会被覆盖。
3. 被覆盖命令可能没有最终 ACK。
4. 这与“说话状态不可被普通命令打断”的目标相反。

### 修改建议

建立单一 `speech_worker`。普通 speak 不抢占当前播放。

建议队列容量为 4。规则如下：

```text
普通 speak：FIFO。
相同 coalesce_key 的低价值提示：只保留最新一个。
安全 stop：允许取消当前播放。
系统 fault：允许取消当前播放。
新普通 speak：不得取消当前播放。
```

播放不是绝对不可中断。应定义为：

```text
普通命令不可中断。
安全命令、用户本地停止、故障恢复可以中断。
```

每个队列项必须保存 `cmd_id`。被合并、丢弃或过期时也必须回 ACK。

---

## 2.3 P0：命令接收和命令执行耦合

`handle_command_response()` 收到 HTTP 命令后，除 speak 外会直接执行：

```text
main/main_command_services.inc:214-270
run_camera_upload_app()
run_find_owner_command()
move_head_to_tracking_angles()
```

sequence 也在当前任务中逐步同步执行。

这意味着 command long-poll 任务可能被阻塞数秒到数十秒。在此期间：

- 无法及时拉取 stop。
- 无法及时拉取 wake 或 sleep。
- 服务端会认为设备在线，但设备不再消费队列。
- sequence 中的网络上传会继续放大阻塞时间。

### 修改建议

命令链路改为：

```text
command ingress
  -> parse and validate
  -> supervisor admission
  -> worker queue
  -> completion event
  -> ACK
```

`command_rx_task` 只负责接收和入队。它不得执行摄像头、舵机和 TTS。

不建议继续支持任意嵌套 sequence。建议二选一：

方案 A，推荐：

```text
server 按 ACK 编排多个原子命令。
```

方案 B：

```text
设备只支持最多 8 步的扁平 ActionPlan。
禁止嵌套。
每一步都检查 deadline 和 cancellation token。
```

---

## 2.4 P0：命令出队即丢失，没有 lease 和重投

server 的 `DeviceCommandQueue.get()` 会直接从队列删除命令：

```text
stack-chan-server/src/server.py:1139-1146
return self._items.pop(index)["command"]
```

如果设备收到命令后重启，或者 ACK 请求失败，该命令不会重新投递。

### 修改建议

server 命令状态改为：

```text
queued
-> leased
-> received
-> running
-> done / failed
```

`leased` 需要 visibility timeout，例如 15 秒。超时未收到 `received` 时重新入队。

设备端保存最近 32 个 `cmd_id` 的结果。重复命令直接返回原结果，不重复执行舵机或 TTS。

ACK 改为 POST JSON，不再使用 GET query：

```http
POST /device/ack
Content-Type: application/json
```

```json
{
  "device_id": "...",
  "cmd_id": "cmd_xxx",
  "state": "running",
  "attempt": 2,
  "message": ""
}
```

---

## 2.5 P0：音频源会在一句话中途切换

当前 `audio_input_task` 根据 `dji.capture_ready` 每轮决定输入源：

```text
main/audio/xiaopai_audio_service.cpp:1539-1569
```

当前 clean queue 中的 PCM 没有 source、timestamp 或 generation。

因此可能出现：

```text
一句录音前半段来自内置麦克风。
DJI 插入并出现首帧。
后半段立即来自 DJI。
```

DJI 拔出时也可能直接切回内置麦克风。

另外，当前源选择只检查 `capture_ready`，没有要求 `identity_confirmed`。任意能够产生 UAC PCM 的设备都可能成为主输入。

### 修改建议

引入 source lease：

```cpp
struct AudioFrame {
    MicSource source;
    uint32_t source_generation;
    uint64_t sample_index;
    int16_t pcm[160];
};
```

规则：

1. Internal mic 从开机开始立即可用。
2. USB Host 在后台启动，不阻塞内置麦克风。
3. DJI 需要 VID/PID 正确，且连续稳定收到 PCM 500 至 1000 ms。
4. 只有 `Monitoring` 状态可以提交源切换。
5. `Recording` 中只设置 `pending_source`。
6. 当前源断开时终止本次 utterance，标记 `source_lost`。不要拼接另一个源。
7. 下一次 utterance 才使用新源。

这比给 `AudioInput` 加普通 mutex 更准确。

---

## 2.6 P0：当前 Quiet/Sleep 不是硬件意义的休眠

原设计中的 `Sleeping / Listening` 仍然：

- 开启 Wi-Fi。
- 开启 USB Host。
- 连续采样麦克风。
- VAD 触发后上传语音到 server。
- 依赖云端或本地 server ASR 判断唤醒词。

它只能叫 `Quiet` 或 `ScreenOff`，不能叫真正的睡眠。

缺点：

1. server 断线时无法语音唤醒。
2. 所有被 VAD 触发的语音都可能上传，存在隐私问题。
3. USB Host 和 Wi-Fi 长期开启，功耗不会显著下降。

### 修改建议

明确三个概念：

```text
Active：正常对话。
Quiet：黑屏或低亮度，本地检测唤醒词，不上传普通语音。
DeepSleep：未来功能。关闭 Wi-Fi 和 USB Host，只能通过触摸或硬件唤醒。
```

Quiet 模式优先使用 ESP-SR WakeNet 或本地关键词检测。

如果当前阶段不实现本地唤醒，就应在产品和代码中命名为 `ScreenOffListening`，不要称为 sleep。

---

## 2.7 P0：内部 I2C 的资源模型不正确

CoreS3 的以下器件共享内部 I2C：

- AXP2101。
- AW9523。
- AW88298。
- ES7210。
- GC0308 的 SCCB 控制接口。
- PY32 扩展控制。
- 触摸和部分 M5 更新逻辑。

当前代码使用 `camera_owns_internal_i2c`。当它为 true 时，主循环不执行 `M5.update()`，灯带和舵机电源操作也会跳过。

问题有两个：

1. 标志在整个相机会话中保持。实际需要严格串行的主要是 camera init、sensor register 和 deinit，而不是 RGB DVP 帧采集全过程。
2. 音频 codec 仍然可以调用 `M5.In_I2C`，所以这个标志没有覆盖所有总线访问。

当前 camera 每拍一张图就 init 和 deinit：

```text
main/main_camera_motion.inc:365-413
```

追踪多轮时会反复重建 camera 和 SCCB device。这会增加延迟和 I2C 恢复风险。

### 修改建议

1. 新建 `internal_i2c_mutex`。
2. 所有 `M5.In_I2C` 访问必须经过统一 wrapper。
3. `M5.update()` 只由 UI task 调用，并经过同一协调机制。
4. camera init、sensor 配置、deinit 必须在锁内完成。
5. camera frame capture 不需要长时间持有 I2C 锁。
6. camera 第一次使用时初始化，之后保持初始化。
7. 空闲较长时间后才由 Supervisor 进入维护窗口并 deinit。
8. camera 恢复失败时，先暂停所有 I2C 客户，再统一重建 bus。不能由音频和 camera 模块分别调用 `M5.In_I2C.begin()`。

最重要的是统一总线恢复入口。当前音频和 camera 都能独立“重建 I2C”，有互相破坏 handle 的风险。

---

## 2.8 P0：DJI USB Host 与供电、调试口存在物理约束

当前代码调用：

```cpp
M5.Power.setUsbOutput(true);
```

这会把 CoreS3 主 USB 口切换为 5 V 输出，用于 Host 供电。

因此必须明确：

1. 该 USB 口作为 Host 接 DJI receiver 时，不能同时作为普通 USB Device 调试口使用。
2. 不能同时依赖该口给 CoreS3 输入供电。
3. 设备应从电池、底座或另一路稳定电源供电。
4. USB receiver、扬声器和舵机同时工作时可能出现电流峰值。
5. 必须做低电压和 brownout 实机测试。

### 修改建议

增加 `PowerManager`：

```text
USB VBUS 上电期间，不启动舵机大动作。
USB 枚举稳定后，再允许高音量 TTS。
低电量时限制音量和舵机速度。
连续 brownout 后禁用 USB 外设并回退内置麦克风。
```

启动顺序改为：

```text
M5 和内部电源
-> 内置麦克风和扬声器
-> 基础 UI
-> Wi-Fi
-> USB Host 后台启动
-> DJI 稳定后在安全点切换
-> 舵机和 camera 按需启动
```

不要为了等待 DJI 而让开机前 5 秒完全没有麦克风。

---

## 2.9 P1：DJI 重采样会产生混叠

当前 48 kHz 到 16 kHz 的处理本质上是按累加器抽样。没有低通滤波。

这会把 8 kHz 以上的频率折叠到语音频段。可能降低 ASR 稳定性。

### 修改建议

使用固定 3:1 FIR decimator：

```text
48 kHz stereo 24-bit
-> 固定声道或混音
-> int32
-> 低通 FIR
-> 每 3 点抽 1 点
-> int16 16 kHz
```

不要在每个 USB callback 中重新选择“能量最大声道”。如果 DJI 左右声道对应两个发射器，当前做法可能在两个人之间跳变。

推荐配置：

```text
left
right
mix
single_auto_once
```

`single_auto_once` 只在连接后校准一次，不逐帧切换。

---

## 2.10 P1：内置麦克风和 DJI 不能共用同一增益及 VAD 阈值

当前 `CONFIG_STACKCHAN_MIC_MAGNIFICATION=8` 会同时作用于内置和 DJI PCM。

DJI 的 24-bit 数据缩放为 int16 后再乘 8，容易削波。内置 ES7210 的底噪和幅度又完全不同。

### 修改建议

使用每个源独立配置：

```cpp
struct MicProfile {
    float digital_gain;
    int vad_start;
    int vad_stop;
    int min_speech_ms;
    int tail_ms;
};
```

默认建议不是固定数值，而是在开机后采集 2 秒环境噪声，计算 noise floor：

```text
start = noise_floor * 4 至 6
stop  = noise_floor * 2 至 3
```

DJI digital gain 初始设为 1。内置麦克风再按实测标定。

---

## 2.11 P1：没有播放后的回声保护

当前：

```text
main/main_app_state.inc
kPostSpeechEchoGuardMs = 0
```

设备播放 TTS 后立即恢复监听。扬声器余音和房间混响可能触发下一次 VAD，造成自问自答。

### 修改建议

第一阶段加入 300 至 600 ms echo guard。

第二阶段如需 barge-in，再启用 AEC。当前 AEC 配置关闭，因此不能宣称支持真正的全双工对话。

---

## 2.12 P1：camera 生命周期过重

每张 QVGA 图像都会：

```text
camera init
等待 500 ms
丢弃旧帧
capture
复制约 153.6 KB
camera deinit
```

追踪时会重复多次。延迟大，也增加内存碎片和总线恢复概率。

### 修改建议

- camera 由 `vision_worker` 唯一拥有。
- 首次使用时初始化。
- 预分配 RGB565 缓冲。
- 连续追踪时复用 camera。
- 空闲 30 至 60 秒后再考虑关闭。
- 上传可选软件 JPEG。QVGA JPEG 通常能明显降低 Wi-Fi 上传量。
- camera 失败恢复必须由 Supervisor 进入 maintenance 状态。

---

## 2.13 P1：舵机软件角度和实际角度不一致

当前 yaw：

```text
yaw zero raw = 460
steps per degree = 3.2
raw clamp = 0 到 1000
软件范围 = -180 到 180 度
```

由 raw 极限推导，当前参数下理论可表示范围约为：

```text
-143.75 度 到 +168.75 度
```

所以 `-180 到 180` 会被 raw clamp。软件仍可能记录为目标角度，导致软件姿态和真实姿态不一致。

当前 UART RX 已配置，但代码只写不读。舵机卡住或掉电后也无法发现。

### 修改建议

1. 每台设备保存 yaw/pitch 的 raw min、zero、raw max。
2. 由 raw 范围推导角度范围。
3. 再叠加机械结构安全范围。
4. 能读取状态包时，读取当前位置和错误状态。
5. 至少实现 ping 和写入 ACK。
6. `tracking_yaw_deg` 只在确认命令成功后更新。
7. 所有 motion 由唯一 `motion_worker` 执行。

camera 内参 `fx=fy=364` 也需要实机标定。不要把经验值当成固定硬件参数。

---

## 2.14 P1：当前久坐检测不能成立

当前逻辑每 5 分钟运行一次 find owner。只要看到人脸就累加：

```text
main/main_camera_motion.inc:1390-1400
```

这不能证明用户坐着。站立、走动但脸仍在画面中，也会被判为久坐。

而且该后台任务当前被注释禁用：

```text
main/main_command_services.inc:536
```

### 修改建议

第一阶段直接改名为：

```text
连续在场提醒
```

真正的久坐检测需要 server 提供至少一种额外证据：

- 上半身或全身姿态。
- 人体框高度和固定机位标定。
- 椅子区域 ROI。
- 连续时间状态。
- 用户离开或明显运动后的 reset。

久坐状态建议保存在 server。设备重启不应清空 25 分钟累计时间。

在没有姿态模型前，不要把 `owner_present` 命名为 `sitting`。

---

## 2.15 P1：通用 NetworkHttp 锁会造成反效果

原设计提出 `NetworkHttp` 资源锁。若 command long-poll 持有该锁 5 秒，TTS 和图片上传会被阻塞。

### 修改建议

不要使用全局 HTTP mutex。

使用：

```text
一个独立 command connection
一个 realtime WebSocket
一个容量为 2 的 bulk HTTP semaphore
```

bulk HTTP 只管理 TTS、image upload、OTA 等大请求。

优先级：

```text
实时 ASR/控制 > TTS > 图片追踪 > OTA和后台日志
```

WebSocket 地址必须由 `/realtime/config` 明确返回。不要依赖“HTTP 端口加 1”。

TTS 建议使用 POST。长文本放在 GET URL 中会增加长度限制、日志暴露和编码问题。

---

## 2.16 P1：表情不应为每种动画动态创建任务

当前 blink、expression animation、temporary expression 会创建独立任务。

停止动画时只等待最多 300 ms。旧任务未及时退出时，新任务可能已经启动。二者共享全局标志和 task handle。

### 修改建议

只保留一个 `ui_task`：

```text
20 至 50 ms tick
-> 更新表情帧
-> 更新 blink deadline
-> 更新临时 overlay deadline
-> 更新灯带 pending state
-> 统一绘制
```

不要为每次表情创建和销毁 FreeRTOS task。

---

## 2.17 P1：安全和凭据问题

当前工程包含硬编码网络凭据和固定 provisioning 密码。控制、音频、图像和命令接口主要使用明文 HTTP/WS。

这会带来：

- LAN 内未授权控制舵机和扬声器。
- 摄像头和语音数据泄露。
- 公共仓库泄露网络凭据。
- 伪造 OTA 或控制命令的风险。

### 修改建议

1. 从源码和 sdkconfig 删除真实凭据。
2. 首次启动只通过 provisioning 写入 NVS。
3. 每台设备生成独立 device token。
4. 命令和 ACK 使用 HMAC 或 TLS。
5. OTA 使用签名校验和 rollback。
6. server 只绑定可信网卡，或要求认证。

---

## 3. 推荐 V2 架构

## 3.1 架构原则

```text
状态只由 Supervisor 修改。
硬件只由对应 owner task 操作。
跨模块只传消息，不直接调用长流程。
固定大小队列优先于动态对象。
普通任务不可抢占 TTS 和录音。
安全事件始终可以取消。
```

## 3.2 模块图

```text
                   Xiaopai Server
          Command / ASR / TTS / Vision / OTA
                           |
                  comms_task / net clients
                           |
                    inbound event queue
                           |
                    supervisor_task
          state + admission + deadlines + policy
             /          |          |          \
      audio_ctrl    speech_job  vision_job  motion_job
          |             |          |          |
 audio_input_task  audio_out   vision_task  motion_task
 internal + DJI     speaker     camera       servo UART
             \          |          |          /
                         ui_task
                display + expression + LEDs
```

内部 I2C 另有统一协调器：

```text
internal_i2c_mutex
+ 单一 bus recovery 入口
+ camera sensor maintenance window
```

## 3.3 推荐任务

| Task | Core | 优先级 | 主要职责 |
|---|---:|---:|---|
| `supervisor_task` | 1 | 6 | 唯一状态机、命令准入、deadline、取消 |
| `audio_input_task` | 0 | 5 | ES7210、DJI、source lease、VAD、pre-roll |
| `audio_output_task` | 1 | 5 | 单一播放通道、tail drain、abort |
| `comms_task` | 0 | 4 | WS、command ingress、ACK、心跳 |
| `vision_task` | 0 | 3 | camera 生命周期、capture、image upload |
| `motion_task` | 0 | 3 | servo UART、动作队列、反馈 |
| `ui_task` | 1 | 2 | 表情、眨眼、灯带、触摸轮询 |
| `health_task` | 0 | 1 | heap、stack、brownout 计数、故障上报 |

不要再为单个表情和临时动作创建任务。

## 3.4 状态和准入

```cpp
struct SystemState {
    PresenceMode presence;
    InteractionState interaction;
    uint32_t generation;
    MicSource active_source;
    MicSource pending_source;
    bool network_ready;
    bool camera_ready;
    bool usb_ready;
    FaultCode fault;
};
```

核心准入：

| 状态 | speak | motion | capture | source switch |
|---|---|---|---|---|
| Active/Monitoring | 执行 | 执行 | 执行 | 可提交 |
| Active/Recording | 延后 | 延后 | 延后 | 只记 pending |
| Active/WaitingReply | 可按策略执行 | 可执行短动作 | 可执行 | 可提交 |
| Active/Speaking | 排队 | 默认延后 | 默认延后 | 只记 pending |
| Quiet/Monitoring | 仅 wake 或安全提示 | 默认拒绝 | 仅后台低频 | 可提交 |
| Fault | 仅恢复和 stop | 回中或断力 | 禁止 | 回退内置 mic |

## 3.5 事件类型

不需要通用动态 EventBus。使用固定 POD：

```cpp
enum class EventType : uint8_t {
    VoiceStarted,
    VoiceEnded,
    SttFinal,
    SpeechFinished,
    UsbAttached,
    UsbReady,
    UsbDetached,
    CommandArrived,
    JobFinished,
    LocalStop,
    Timeout,
    Fault
};
```

建议：

```text
SupervisorQueue：32 项。
SpeechQueue：4 项。
VisionQueue：3 项。
MotionQueue：8 项。
UIQueue：8 项，允许覆盖低优先级 overlay。
```

PCM 不进入 EventBus。使用 StreamBuffer 或固定 block pool。

---

## 4. 关键流程

## 4.1 启动

```text
init NVS
-> init M5 和 internal I2C
-> init Supervisor 和 UI
-> init internal audio immediately
-> start audio input/output
-> connect Wi-Fi and server
-> start realtime channel
-> start USB Host in background
-> wait DJI identity + stable PCM
-> set pending source
-> switch at Monitoring boundary
-> camera and servo lazy init
```

## 4.2 DJI 插入

```text
USB attached
-> enumerate
-> verify VID/PID and UAC descriptors
-> receive stable PCM
-> calibrate channel and noise floor
-> emit UsbReady
-> if Monitoring, commit source generation
-> otherwise set pending source
```

## 4.3 DJI 拔出

```text
USB detached
-> invalidate DJI generation
-> if Recording on DJI:
     stop utterance with source_lost
     do not append internal PCM
-> set pending InternalMic
-> switch at Monitoring
```

## 4.4 speak

```text
CommandArrived
-> server deadline check
-> Supervisor admission
-> SpeechQueue
-> Speaking
-> stream TTS with read timeout
-> drain speaker 80 ms
-> echo guard 400 ms
-> SpeechFinished
-> Monitoring
```

安全 stop 可以设置 cancellation generation，并清空播放 queue。

## 4.5 camera 和追踪

```text
VisionJob
-> admission
-> optional move to pose through MotionJob
-> capture with persistent camera
-> optional JPEG encode
-> upload
-> return face center
-> Supervisor decides next MotionJob
```

不要让 `vision_task` 直接调用 motion 函数。这样不会形成 camera、UART 和 I2C 的嵌套锁。

## 4.6 Quiet 唤醒

推荐：

```text
Quiet/Monitoring
-> local WakeNet
-> wake candidate
-> optional short confirmation ASR
-> Active/WaitingReply
-> play cached wake reply
```

server 不在线时仍能唤醒和播放本地缓存回复。

---

## 5. Server V2

建议拆分：

```text
src/
  app.py
  device_registry.py
  command_store.py
  command_api.py
  realtime_gateway.py
  asr_service.py
  tts_service.py
  vision_service.py
  ota_service.py
  policy.py
  schemas.py
```

`command_store.py` 负责：

- priority。
- TTL。
- coalesce。
- lease。
- retry。
- ACK history。
- idempotency。

建议命令 schema：

```json
{
  "cmd_id": "cmd_xxx",
  "type": "speak",
  "priority": 50,
  "created_at_ms": 0,
  "deadline_ms": 0,
  "coalesce_key": "",
  "safety_class": "normal",
  "admission": {
    "allow_in_quiet": false,
    "defer_during_recording": true,
    "defer_during_speaking": true
  },
  "payload": {
    "text": "你好"
  }
}
```

不再使用含义模糊的 `interrupt: true`。

---

## 6. 落地顺序

### 阶段 1：先解决一致性

1. 新建 Supervisor。
2. 所有状态改为 Supervisor 单写。
3. 删除 `voice_recording_active` 等重复事实来源。
4. command rx 只入队，不执行。
5. speak 不再 preempt 和 overwrite。

### 阶段 2：音频源稳定

1. 内置 mic 立即启动。
2. 引入 source generation。
3. Recording 固定 source lease。
4. DJI 要求 identity confirmed。
5. 使用 FIR 3:1 decimator。
6. 分离 source gain 和 VAD profile。

### 阶段 3：命令可靠性

1. server 加 lease 和 retry。
2. device 加 cmd_id 去重。
3. ACK 改 POST。
4. sequence 改 server 编排或限制为扁平计划。

### 阶段 4：I2C、camera 和 servo

1. 统一 I2C recovery。
2. camera 持久化。
3. UI 单任务。
4. motion 单任务。
5. 修正 yaw 范围。
6. 加 servo ping 和可选位置反馈。

### 阶段 5：Quiet 和久坐

1. 本地 wake word。
2. 将 sleep 重命名为 Quiet。
3. 久坐逻辑移到 server。
4. 没有姿态模型前只做“连续在场提醒”。

### 阶段 6：安全和量产

1. 删除硬编码凭据。
2. device token。
3. TLS 或 HMAC。
4. 签名 OTA 和 rollback。
5. 电源和 brownout 策略。

---

## 7. 必须通过的验收测试

1. DJI 在录音中插入。当前 utterance 不能混入 DJI PCM。
2. DJI 在录音中拔出。utterance 以 `source_lost` 结束，下一次使用内置 mic。
3. 录音中连续下发 3 条 speak。不得打断录音，不得无 ACK 丢失。
4. speak 中下发普通 speak。当前 speak 完整播放，后续按策略排队或合并。
5. speak 中下发本地 stop。应在 200 ms 级别停止。
6. 设备收到命令后立即重启。server 应在 lease 超时后重投，device 去重。
7. camera 连续拍 1000 张。不得反复 I2C 重建，不得出现 heap 持续下降。
8. camera、扬声器、USB receiver 和舵机组合运行。不得 brownout。
9. server 断线时 Quiet 模式仍能本地唤醒。
10. 连续运行 8 小时。监控最小 internal heap、DMA largest block、PSRAM largest block 和任务 stack watermark。
11. 舵机命令超出物理范围。软件目标和 raw 目标必须一致地 clamp。
12. 没有 sitting 模型时，接口不得返回虚假的 `sitting=true`。

---

## 8. 最终建议

原设计中值得保留的部分：

- 将 server 命令交给设备做准入。
- 录音期间不执行普通 speak。
- 音频 PCM 走专用 buffer，不走普通事件队列。
- camera、motion、speech 分 worker。
- 命令增加 TTL、priority 和 coalesce。

需要替换的部分：

```text
通用 EventBus + 大量资源锁
```

替换为：

```text
Supervisor 单写状态
+ 硬件 owner task
+ 固定消息队列
+ source lease
+ command lease
+ safety cancellation
```

这套方案更符合 ESP32-S3 的运行方式。它减少共享变量、嵌套锁、动态任务和重复初始化。也更容易做长时间 soak test 和故障恢复。
