# Xiaopai / Stack-chan V3 总体架构与落地方案

版本：V3.0  
日期：2026-07-10  
适用范围：CoreS3 机器人固件、Xiaopai Server、OpenClaw 工作助理插件、飞书及外部服务适配器

---

## 0. 方案目标

本方案用于实现一个可长期在线运行的桌面工作助理机器人。系统需要同时支持语音交互、主动提醒、飞书业务操作、低频视觉感知、头部动作、表情展示和安全 OTA。

最终用户能力包括：

1. 用户通过本地唤醒词或触摸进入语音交互。
2. 机器人完成实时 ASR、业务理解、工具调用和 TTS 回复。
3. OpenClaw 查询和创建飞书日程。
4. OpenClaw 发送会议迟到通知。
5. 系统主动播报日程、会议、外勤和出差提醒。
6. 机器人低频寻找用户并朝向用户。
7. 系统根据在场和姿态信息进行健康提醒。
8. DJI USB 麦克风和内置麦克风可以稳定切换。
9. 网络、USB、摄像头或服务端异常后可以自动恢复。
10. 主动提醒具备持久化、重试、去重和真实送达确认。
11. 固件支持签名 OTA、启动确认和回滚。
12. 设备可以完成至少 7 天连续运行测试。

本方案不将以下能力列为 V3 必须目标：

1. 高频实时视觉跟踪。
2. 本地大模型推理。
3. 无 AEC 条件下的全双工语音打断。
4. 仅凭人脸存在判断用户久坐。
5. 在同一个 USB-C 口同时进行 USB Host、USB Device 调试和设备供电。

---

## 1. 系统分层

系统分为三层。

```text
OpenClaw 业务层
  日程、会议、出行、天气、健康关怀、短期上下文、工具调用
                           |
                           v
Xiaopai Server 控制与交付层
  设备注册、命令持久化、租约、ACK、ASR、TTS、视觉、OTA
                           |
                           v
CoreS3 设备执行层
  本地唤醒、录音、播放、摄像头、舵机、表情、触摸、健康监控
```

### 1.1 OpenClaw 业务层职责

OpenClaw 负责：

- 理解用户输入和系统事件。
- 查询飞书日历和通讯录。
- 创建日程和邀请参会人。
- 发送飞书通知。
- 查询天气和路线。
- 生成日程复盘、会议提醒、出行建议和健康关怀话术。
- 维护当前会议、最近创建日程等短期上下文。
- 输出标准化 `StructuredResponse`。
- 记录外部写操作的幂等结果。

OpenClaw 不直接操作摄像头、舵机、扬声器和屏幕。

### 1.2 Xiaopai Server 职责

Xiaopai Server 负责：

- 保存设备和命令状态。
- 把结构化展示计划转换成设备原子命令。
- 维护控制 WebSocket。
- 维护实时音频 WebSocket。
- 提供 TTS、图像、OTA 等大数据 HTTP 服务。
- 管理命令租约、续租、重试、去重和超时。
- 汇总设备 ACK，并向 OpenClaw 回传真实交付结果。
- 执行 ASR、TTS 和视觉模型。
- 保存主动提醒的交付状态。

### 1.3 CoreS3 设备职责

设备负责：

- 本地唤醒词检测。
- 录音、VAD、音频源选择和实时音频上传。
- TTS 音频播放。
- 摄像头采集和图片上传。
- 舵机动作。
- 表情、灯带和触摸。
- 命令准入、取消和执行。
- 硬件错误恢复。
- 运行资源监控。
- OTA 下载、校验、切换和回滚确认。

---

## 2. 核心架构原则

### 2.1 单一状态所有者

系统运行状态只允许由 `supervisor_task` 修改。

其他任务只能：

- 接收作业。
- 操作自己拥有的硬件。
- 向 Supervisor 返回事件。

任何任务都不能直接修改全局交互状态。

### 2.2 单一硬件所有者

| 硬件或资源 | 唯一所有者 |
|---|---|
| 内置麦克风和音频输入选择 | `audio_input_task` |
| 扬声器和音频输出 | `speech_task` |
| 摄像头 | `vision_task` |
| 舵机 UART | `motion_task` |
| 屏幕、表情、灯带和触摸 | `ui_task` |
| 控制连接 | `control_transport_task` |
| 实时 ASR 连接 | `realtime_audio_task` |
| 系统状态 | `supervisor_task` |

USB Host 组件保留其内部任务。USB 回调只写入固定环形缓冲，不执行网络、ASR 或业务逻辑。

### 2.3 固定消息和固定缓冲

任务间使用固定大小 POD 消息。

音频热路径使用固定块池和环形缓冲。禁止在每个音频帧中反复调用 `malloc`、`free` 或创建 `std::vector`。

摄像头上传直接读取 framebuffer。禁止为每张图再复制完整图像。

### 2.4 控制链路和大数据链路分离

控制消息必须始终保持低延迟。

系统使用三条链路：

```text
Control WebSocket
  命令、ACK、心跳、状态、取消、租约续期

Realtime Audio WebSocket
  Opus 或 PCM 音频帧、ASR 会话状态、识别结果

Bulk HTTP
  TTS 音频、图像上传、OTA、诊断文件
```

图片上传、TTS 下载和 OTA 不得阻塞控制 WebSocket。

### 2.5 持久化后再投递

主动提醒和设备命令必须先写入持久化存储，再发送给设备。

“已生成”“已入队”“设备已收到”和“用户已听到”是不同状态。

### 2.6 普通任务不抢占，安全事件可以取消

普通语音、表情、动作和视觉任务按准入策略排队。

以下事件可以取消当前作业：

- 用户本地停止。
- 远程安全停止。
- USB 或音频源丢失。
- 硬件故障。
- OTA 维护切换。
- Supervisor 超时。
- 当前会话被新的用户会话替换。

---

## 3. 总体模块图

```text
                         OpenClaw
       Intent Router / Domain Skills / Tool Adapters
             |                  |                 |
       飞书日历与 IM          天气与路线        上下文和幂等
             \                  |                 /
                      StructuredResponse
                              |
                     Render Plan Adapter
                              |
                              v
                    Xiaopai Delivery API
                              |
                    Persistent Command Store
                              |
        +---------------------+----------------------+
        |                     |                      |
 Control WebSocket     Realtime Audio WS        Bulk HTTP
        |                     |                      |
        v                     v                      v
 control_transport     realtime_audio_task      speech/vision/OTA
        \                     |                      /
         \                    |                     /
                      supervisor_task
        +----------+----------+-----------+----------+
        |          |                      |          |
 audio_input   speech_task            vision_task  motion_task
        \          |                      |          /
                    ui_task / health_task
```

---

## 4. 设备固件任务设计

### 4.1 常驻任务

| Task | 建议 Core | 优先级 | 初始栈 | 职责 |
|---|---:|---:|---:|---|
| `supervisor_task` | 1 | 6 | 6 KiB | 状态机、准入、超时、取消、作业分发 |
| `audio_input_task` | 1 | 6 | 6 KiB | 内置麦克风、DJI PCM、VAD、pre-roll、source lease |
| `control_transport_task` | 0 | 5 | 8 KiB | 控制 WebSocket、ACK、心跳、命令接收 |
| `realtime_audio_task` | 0 | 5 | 10 KiB | ASR WebSocket、Opus 上传、识别结果 |
| `speech_task` | 1 | 5 | 10 KiB | TTS 请求、流式播放、扬声器控制 |
| `vision_task` | 0 | 3 | 10 KiB | camera 生命周期、采集、上传、视觉结果 |
| `motion_task` | 无固定或 0 | 3 | 4 KiB | 舵机控制、反馈、动作队列 |
| `ui_task` | 1 | 2 | 6 KiB | 表情、眨眼、灯带、触摸、屏幕刷新 |
| `health_task` | 无固定 | 1 | 3 KiB | heap、stack、brownout、故障统计 |

应用自行创建的常驻任务栈目标不超过 72 KiB。USB Host、Wi-Fi、lwIP、FreeRTOS 和驱动任务不计入该数值，但必须计入实机内部 SRAM 验收。

任务栈以 `uxTaskGetStackHighWaterMark()` 的实测结果调整。每个任务必须保留至少 1 KiB 且不少于 20% 的空闲栈。

### 4.2 任务间队列

| 队列 | 容量 | 满时策略 |
|---|---:|---|
| `SupervisorQueue` | 32 | 安全事件优先。重复状态事件合并 |
| `SpeechQueue` | 4 | 过期任务丢弃。同 `coalesce_key` 只保留最新 |
| `VisionQueue` | 3 | 同类拍照任务合并。保留最新请求 |
| `MotionQueue` | 8 | `stop` 优先。过期普通动作丢弃 |
| `UIQueue` | 8 | 状态展示只保留最新。临时 overlay 可覆盖 |
| `AckQueue` | 16 | 不丢终态 ACK。必要时写入本地重放缓存 |

所有被丢弃、合并、取消或过期的命令都必须产生终态 ACK。

### 4.3 音频块池

默认音频帧为 20 ms、16 kHz、单声道、16 bit。

```cpp
struct AudioFrameRef {
    uint16_t block_index;
    uint16_t sample_count;
    MicSource source;
    uint32_t source_generation;
    uint64_t sample_index;
    uint64_t monotonic_us;
};
```

建议配置：

```text
单块 PCM：320 samples，640 bytes
固定块数量：64
总 PCM 块池：约 40 KiB
存储位置：PSRAM
I2S DMA buffer：内部 SRAM
pre-roll：默认 500 ms
```

块池耗尽时，优先保留当前录音和控制数据。低价值监听帧可以丢弃，并记录计数。

---

## 5. 状态机

### 5.1 系统模式

```cpp
enum class SystemMode {
    Booting,
    Active,
    Quiet,
    Maintenance,
    Fault
};
```

含义：

- `Booting`：硬件和网络初始化。
- `Active`：正常对话和主动提醒。
- `Quiet`：屏幕关闭或低亮度。只运行本地唤醒和必要健康监控。
- `Maintenance`：I2C 恢复、OTA、摄像头重建等维护操作。
- `Fault`：关键硬件不可用。只接受 stop、诊断和恢复命令。

### 5.2 交互状态

```cpp
enum class InteractionState {
    Monitoring,
    Recording,
    WaitingReply,
    Speaking
};
```

含义：

- `Monitoring`：等待本地唤醒、触摸、主动提醒或远程命令。
- `Recording`：正在采集一个固定音频源的 utterance。
- `WaitingReply`：ASR 完成，等待 OpenClaw 或 server 返回结果。
- `Speaking`：正在播放 TTS。

### 5.3 状态约束

1. 同一时刻只能存在一个主交互状态。
2. `Recording` 期间锁定当前麦克风源。
3. `Speaking` 期间普通 VAD 不启动新录音。
4. `Quiet` 模式只允许 `Monitoring`。
5. `Maintenance` 模式不接收普通视觉、动作和语音作业。
6. 普通 speak 不打断当前 speak。
7. 本地 stop、安全 stop 和故障事件可以打断 speak。
8. 麦克风源只在 `Monitoring` 状态提交切换。
9. 过期完成事件不得改变当前状态。
10. 每次取消或会话切换都递增 cancellation generation。

### 5.4 作业头

```cpp
struct JobHeader {
    char cmd_id[40];
    uint32_t job_id;
    uint32_t boot_id;
    uint32_t state_generation;
    uint32_t cancellation_generation;
    uint32_t source_generation;
    uint32_t turn_id;
    uint64_t deadline_tick;
};
```

Worker 返回结果时必须携带原始 `JobHeader`。

Supervisor 只接受以下结果：

```cpp
event.boot_id == current_boot_id
event.state_generation == current_state_generation
event.cancellation_generation == current_cancellation_generation
event.deadline_tick >= now_tick
```

不符合条件的结果记录为 stale，并直接忽略。

设备内部的 timeout 使用单调时钟。服务端保存 UTC 时间。服务端向设备下发 `ttl_ms`，设备在收到命令时计算单调 deadline。

---

## 6. 命令协议和可靠性

### 6.1 控制连接握手

设备连接 Control WebSocket 后发送：

```json
{
  "type": "hello",
  "device_id": "xiaopai_xxx",
  "boot_id": 12345,
  "firmware_version": "0.3.0",
  "protocol_version": 3,
  "capabilities": [
    "internal_mic",
    "dji_uac",
    "camera",
    "servo",
    "local_wake",
    "signed_ota"
  ],
  "last_ack_seq": 1024
}
```

服务端返回：

```json
{
  "type": "hello_ack",
  "server_time": "2026-07-10T10:00:00+08:00",
  "heartbeat_interval_ms": 5000,
  "lease_ms": 15000,
  "device_config_version": 12
}
```

### 6.2 命令结构

```json
{
  "cmd_id": "cmd_xxx",
  "type": "speak",
  "priority": 50,
  "ttl_ms": 30000,
  "attempt": 1,
  "coalesce_key": "",
  "safety_class": "normal",
  "turn_id": "turn_xxx",
  "admission": {
    "allow_in_quiet": false,
    "defer_during_recording": true,
    "defer_during_speaking": true,
    "presence_requirement": "preferred"
  },
  "payload": {
    "text": "五分钟后有项目会议。"
  }
}
```

字段要求：

- `cmd_id` 全局唯一。
- `ttl_ms` 从设备收到命令时开始计算。
- `priority` 只在同一安全等级内排序。
- `coalesce_key` 只用于低价值、可覆盖的提示。
- `turn_id` 用于取消旧会话剩余语音。
- `safety_class` 取值为 `normal`、`local_stop`、`safety_stop`、`fault_recovery`。
- `presence_requirement` 取值为 `required`、`preferred`、`none`。

不再使用含义不明确的 `interrupt: true`。

### 6.3 命令状态

```text
queued
-> leased
-> received
-> running
-> rendered / done / failed / cancelled / expired
```

含义：

- `queued`：命令已持久化。
- `leased`：命令已发送给某个设备实例。
- `received`：设备已保存命令并完成去重检查。
- `running`：设备已开始执行。
- `rendered`：用户可感知的语音或展示已经完成。
- `done`：非展示类动作已完成。
- `failed`：执行失败。
- `cancelled`：被 stop、会话切换或安全策略取消。
- `expired`：超过业务 deadline。

设备应在 3 秒内返回 `received`。

运行超过 10 秒的作业每 5 秒发送一次 `running` 续租。默认 lease 为 15 秒。OTA 和长 TTS 使用命令级 lease 配置。

### 6.4 ACK 结构

```json
{
  "type": "command_ack",
  "ack_seq": 1025,
  "device_id": "xiaopai_xxx",
  "boot_id": 12345,
  "cmd_id": "cmd_xxx",
  "attempt": 1,
  "state": "rendered",
  "effect": "speech_played",
  "started_at_tick": 112233,
  "finished_at_tick": 115900,
  "message": ""
}
```

终态 ACK 必须持久保存到服务端。

控制连接断开时，设备将未发送的终态 ACK 放入固定重放缓存。重新连接后从 `last_ack_seq` 继续发送。

### 6.5 去重

设备维护两级去重：

1. RAM LRU 保存最近 32 至 64 个普通命令。
2. NVS 循环日志保存最近 16 至 32 个非幂等命令结果。

非幂等命令包括：

- 会导致明显机械动作的命令。
- 会改变持久状态的命令。
- 需要防止重启后重复执行的命令。

重复命令直接返回已保存结果，不再次执行。

### 6.6 ActionPlan

服务端默认按 ACK 编排多个原子命令。

设备只保留受限的扁平 `ActionPlan`，用于一个短展示中的表情、动作和语音协调。

限制如下：

```text
最多 8 步
禁止嵌套
禁止包含 stop
总时长不超过 30 秒
每一步都有 deadline
每一步都检查 cancellation generation
```

时间敏感的主动提醒不把 `find_owner` 作为成功前置条件。`find_owner` 仅作为限时的 best effort 前奏。

---

## 7. 音频输入设计

### 7.1 启动顺序

```text
M5 和内部电源初始化
-> 内置 ES7210 初始化
-> audio_input_task 启动
-> UI 和基础交互可用
-> Wi-Fi 和 server 连接
-> USB Host 后台启动
-> DJI 完成身份确认和稳定采样
-> 在 Monitoring 边界提交音频源切换
```

设备启动后始终先具备内置麦克风能力。USB 初始化不得造成开机阶段无可用麦克风。

### 7.2 音频源状态

```cpp
enum class MicSource {
    InternalMic,
    DjiMic
};
```

```cpp
struct AudioSourceState {
    MicSource active_source;
    MicSource pending_source;
    uint32_t source_generation;
    bool dji_identity_confirmed;
    bool dji_capture_ready;
    uint32_t dji_stable_ms;
};
```

DJI 成为候选输入源需要同时满足：

1. VID 和 PID 匹配。
2. UAC descriptor 符合预期。
3. PCM 格式正确。
4. 连续稳定收到 PCM 500 至 1000 ms。默认 800 ms。
5. 连接期间没有持续丢包或异常重启。

### 7.3 Source lease

每个 utterance 在开始时获取 source lease。

录音期间：

- 不切换源。
- DJI 插入只更新 `pending_source`。
- 当前 DJI 拔出时立即结束 utterance。
- 结束原因标记为 `source_lost`。
- 不把内置麦克风 PCM 拼接到同一句语音。
- 下一次 utterance 使用新的 active source。

每次源切换递增 `source_generation`。

### 7.4 DJI 重采样

处理链路固定为：

```text
48 kHz stereo 24-bit
-> 固定声道策略
-> int32 归一化
-> 低通 FIR
-> 3:1 decimation
-> 16 kHz mono int16
```

声道策略支持：

- `left`
- `right`
- `mix`
- `single_auto_once`

`single_auto_once` 只在连接稳定后的校准阶段选择一次声道。运行期间不按每帧能量反复切换。

### 7.5 每源音频配置

```cpp
struct MicProfile {
    float digital_gain;
    int32_t vad_start;
    int32_t vad_stop;
    uint16_t min_speech_ms;
    uint16_t tail_ms;
    uint16_t pre_roll_ms;
};
```

内置麦克风和 DJI 使用独立 profile。

默认流程：

1. 每次冷启动或新音频源首次启用时采集 2 秒环境噪声。
2. 计算 noise floor。
3. 设置 VAD start 为 noise floor 的 4 至 6 倍。
4. 设置 VAD stop 为 noise floor 的 2 至 3 倍。
5. DJI digital gain 默认从 1 开始。
6. 内置麦克风按实机标定设置增益。
7. 保存稳定配置到 NVS。

### 7.6 VAD 和 pre-roll

实时 ASR 和本地 WAV 路径共用同一个 pre-roll 环形缓冲。

默认参数：

```text
pre-roll：500 ms
min speech：250 ms
tail：500 至 800 ms
最大单次录音：15 s
```

VAD 触发后，先发送 pre-roll，再发送当前和后续音频。短唤醒词和首字不得被截断。

### 7.7 Quiet 模式和本地唤醒

Quiet 模式执行：

```text
屏幕关闭或低亮度
普通语音不上传
本地 WakeNet 或关键词模型持续运行
触摸仍可唤醒
命中唤醒词后切换到 Active
播放本地缓存确认音
开始正常录音和 ASR
```

自定义“小派同学”唤醒词作为独立模型资产管理。模型必须通过误唤醒率、漏唤醒率、内存和 CPU 验收后再设为默认。

DeepSleep 是独立功能。DeepSleep 关闭 Wi-Fi 和 USB Host，只能通过触摸、RTC 或硬件源唤醒。Quiet 和 DeepSleep 不共用产品名称。

---

## 8. 语音输出设计

### 8.1 SpeechQueue 规则

- 普通 speak 按 FIFO 执行。
- 普通 speak 不打断当前 speak。
- 相同 `coalesce_key` 的低价值提示只保留最新一条。
- 新 `turn_id` 可以取消旧 turn 尚未播放的语音。
- `local_stop`、`safety_stop` 和 `fault_recovery` 可以取消当前播放。
- 被取消和被合并命令必须返回终态 ACK。

### 8.2 TTS 请求

TTS 使用 POST。

```http
POST /v3/tts
Content-Type: application/json
```

```json
{
  "device_id": "xiaopai_xxx",
  "cmd_id": "cmd_xxx",
  "text": "五分钟后有项目会议。",
  "voice": "default",
  "sample_rate": 16000
}
```

服务端流式返回音频。设备边接收边播放，不构造完整 WAV 副本。

### 8.3 播放完成语义

`speech_task` 在以下条件满足后返回 `rendered`：

1. TTS HTTP 流正常结束。
2. 所有 PCM 已写入扬声器。
3. 输出队列已 drain。
4. 扬声器尾部保护时间结束。

`rendered` 表示设备完成了可感知播放。它不表示用户一定理解或响应。

### 8.4 回声保护

默认半双工。

播放完成后设置 400 ms echo guard。配置范围为 300 至 600 ms。

V3 不启用普通语音 barge-in。用户可通过触摸 stop 或远程安全 stop 停止播放。

需要全双工时，必须先完成 AEC、回声参考、扬声器延迟和实际房间测试。

---

## 9. 摄像头和视觉设计

### 9.1 Camera 生命周期

`vision_task` 唯一拥有 camera。

流程：

```text
首次视觉作业
-> 初始化 camera
-> 丢弃初始化旧帧
-> 执行拍照和连续短流程
-> 保持 camera ready
-> 空闲 60 秒后申请 Maintenance
-> 关闭 camera
```

追踪过程中不反复 init 和 deinit。

### 9.2 图像缓冲

默认使用 QVGA RGB565。

上传方式：

```text
直接持有 framebuffer
-> 以 16 KiB 分块写入 Bulk HTTP
-> 上传完成
-> 归还 framebuffer
```

不创建第二份 153.6 KiB 完整图像副本。

服务端请求中携带：

```json
{
  "width": 320,
  "height": 240,
  "pixel_format": "rgb565",
  "rotation": 0,
  "capture_id": "cap_xxx"
}
```

软件 JPEG 作为后续可配置优化。只有在实测网络收益大于 CPU 和内存开销时启用。

### 9.3 Find Owner

`find_owner` 是低频离散视觉流程。

默认流程：

```text
检查 presence policy
-> 按校准后的安全角度扫描
-> 每个位置拍一张
-> server 返回 face_found、center、confidence
-> 计算下一次头部调整
-> 最多 3 轮
-> 返回 found 或 not_found
```

限制：

```text
总时长不超过 8 秒
每轮都检查取消 token
视觉失败不阻塞控制通道
没有检测到人脸时返回明确的 not_found
```

成功话术只在 `face_found=true` 时播放。

对于会议提醒等时间敏感任务：

- `presence_requirement=preferred` 时，寻找用户失败后仍执行播报或走备用通知。
- `presence_requirement=required` 时，不公开播报敏感内容，改用飞书或其他私密通道。

V3 的视觉目标是低频寻找和朝向用户，不是实时平滑跟踪。

---

## 10. 舵机和动作设计

### 10.1 标定数据

每台设备保存：

```cpp
struct ServoCalibration {
    int raw_min;
    int raw_zero;
    int raw_max;
    float steps_per_degree;
    float safe_min_degree;
    float safe_max_degree;
    float max_speed;
    float max_acceleration;
};
```

角度范围由 raw 范围和机械安全范围共同决定。

动作命令先 clamp，再计算 raw。软件状态只记录实际发送且确认成功的目标值。

### 10.2 MotionQueue

`motion_task` 串行执行所有动作。

支持：

- 绝对角度。
- 相对角度。
- 回中。
- 点头计划。
- 安全停止。
- 断力或限制力矩。

每步动作包含：

- target。
- duration。
- deadline。
- cancellation generation。
- expected feedback。

### 10.3 反馈

优先实现：

1. servo ping。
2. 写入 ACK。
3. 当前位置读取。
4. 错误状态读取。

无法读取位置时，结果标记为 `completed_unverified`，不能假装获得了真实位置反馈。

### 10.4 动作安全

- USB 枚举期间不启动大幅度舵机动作。
- 扬声器高音量和双舵机启动尽量错峰。
- 动作超时后立即停止继续发送。
- 连续通信失败后进入 Fault。
- 用户本地 stop 优先级最高。
- 超出标定范围的动作在入队前拒绝。

---

## 11. UI、表情和触摸

`ui_task` 使用 20 至 50 ms tick。

它负责：

- 当前系统表情。
- 临时 overlay。
- 眨眼 deadline。
- 嘴型或静态口型。
- 灯带状态。
- 触摸检测。
- 屏幕亮度。
- Quiet 和 Fault 页面。
- 内存和连接诊断页面。

每次表情变化只更新状态，不创建新的 FreeRTOS task。

UI 优先级低于音频和控制任务。复杂动画必须可以跳帧，不能阻塞音频。

触摸默认映射：

| 操作 | 行为 |
|---|---|
| 单击或摸头 | Active 状态下触发日程入口或确认 |
| Quiet 下触摸 | 本地唤醒 |
| 长按 | 本地 stop |
| 维护组合操作 | 进入 provisioning 或诊断模式 |

---

## 12. 内部 I2C 协调

CoreS3 内部 I2C 访问统一经过 `BoardI2cService`。

```cpp
class BoardI2cService {
public:
    Result transact(..., uint32_t timeout_ms);
    Result enter_maintenance(...);
    Result recover_bus(...);
    void leave_maintenance();
};
```

规则：

1. 所有内部 `M5.In_I2C` 操作使用同一个 wrapper。
2. 普通事务使用有界 mutex 和 timeout。
3. camera sensor init、配置和 deinit 在短事务内持锁。
4. DVP frame capture 不长时间持有 I2C 锁。
5. `M5.update()` 只由 `ui_task` 调用，并遵守维护门控。
6. 音频、camera 和 UI 模块不能自行调用 bus begin 或重建 handle。
7. I2C 恢复只允许在 `Maintenance` 模式执行。
8. 恢复前暂停所有 I2C 客户。
9. 恢复后按固定顺序重新探测电源、扩展 IO、codec、camera sensor 和触摸。
10. 恢复失败进入 Fault，并上报具体设备。

---

## 13. 电源和 USB Host

### 13.1 USB 物理边界

DJI receiver 连接时，CoreS3 主 USB 口工作在 Host 和 5 V 输出模式。

该端口此时：

- 不作为普通 USB Device 调试口。
- 不作为 CoreS3 的主要输入供电口。
- 不与 PC 同时连接为设备模式。

设备使用电池、底座或独立稳定电源供电。

### 13.2 PowerManager

`PowerManager` 维护：

```text
battery level
VBUS state
USB attach state
speaker load level
servo activity
brownout count
thermal or repeated fault state
```

策略：

- USB VBUS 上电后等待枚举稳定。
- 枚举期间不执行舵机大动作。
- 舵机启动时限制瞬时扬声器音量。
- 低电量时限制音量、舵机速度和摄像头频率。
- 连续 brownout 后禁用 USB 外设，并回退内置麦克风。
- 关键电源故障写入 NVS 计数。
- Health 上报包含 reset reason 和 brownout 统计。

### 13.3 启动顺序

```text
NVS
-> M5 和内部电源
-> 内置音频
-> Supervisor 和 UI
-> Wi-Fi
-> Control WebSocket
-> Realtime Audio WebSocket
-> USB Host
-> DJI 稳定切换
-> camera 和 servo 按需初始化
```

---

## 14. Flash、PSRAM 和内部 SRAM 规划

### 14.1 分区表

16 MB Flash 建议分区：

```csv
# Name,    Type, SubType, Offset,   Size
nvs,       data, nvs,      0x9000,   0x10000
otadata,   data, ota,      0x19000,  0x2000
phy_init,  data, phy,      0x1B000,  0x1000
coredump,  data, coredump, 0x1C000,  0x20000
ota_0,     app,  ota_0,    0x40000,  0x700000
ota_1,     app,  ota_1,    0x740000, 0x700000
assets,    data, spiffs,   0xE40000, 0x1C0000
```

用途：

- NVS：Wi-Fi、token、标定、去重、健康计数。
- coredump：崩溃诊断。
- 双 7 MB OTA：保留足够固件增长空间。
- assets：唤醒模型、缓存提示音、只读资源和诊断配置。

### 14.2 PSRAM 规划

以下对象放入 PSRAM：

- 音频固定块池。
- DJI raw ring。
- DJI PCM ring。
- camera framebuffer。
- 可选 ASR 和 TTS 网络缓冲。
- 表情资源。
- 唤醒模型允许放置的外部内存部分。

以下对象保留在内部 SRAM：

- I2S DMA。
- camera DMA 所需块。
- ISR 数据。
- SupervisorQueue 和高优先级小消息。
- FreeRTOS 关键控制结构。
- 小型硬件状态。
- 对实时性敏感的栈。

### 14.3 内存验收线

以下数值作为 V3 工程验收线：

```text
最坏场景最低内部空闲内存        >= 64 KiB
最大内部连续块                  >= 48 KiB
最大 DMA 连续块                 >= 32 KiB
最大 PSRAM 连续块               >= 512 KiB
每个任务剩余栈                  >= 1 KiB
每个任务剩余栈比例              >= 20%
应用常驻任务栈总预算            <= 72 KiB
音频热路径动态分配次数          = 0
连续拍照后 PSRAM 持续下降       = 0
```

Health task 周期记录：

- current free internal heap。
- minimum free internal heap。
- largest internal block。
- largest DMA block。
- current free PSRAM。
- largest PSRAM block。
- 每个任务 stack watermark。
- audio block pool high watermark。
- queue high watermark。
- dropped frame 和 stale event 计数。

---

## 15. Xiaopai Server V3

### 15.1 模块结构

```text
src/
  app.py
  device_registry.py
  control_gateway.py
  audio_gateway.py
  command_store.py
  command_dispatcher.py
  delivery_coordinator.py
  asr_service.py
  tts_service.py
  vision_service.py
  posture_service.py
  ota_service.py
  policy.py
  schemas.py
  database.py
```

### 15.2 持久化

使用 SQLite WAL。

核心表：

```text
devices
device_sessions
commands
command_attempts
command_acks
deliveries
captures
ota_releases
```

命令必须在写入 `commands` 后才允许投递。

每次设备连接创建 `device_session`，包含 `boot_id`、固件版本、能力和在线状态。

### 15.3 设备选择

每个用户或 OpenClaw session 显式绑定 `device_id`。

只有在以下条件同时满足时才允许自动选择设备：

1. 当前用户只绑定一个设备。
2. 该设备 Control WebSocket 在线。
3. 最近心跳在 TTL 内。
4. 设备能力满足当前命令。

不得回退到历史默认设备或离线设备。

### 15.4 重试策略

命令级策略包含：

```json
{
  "max_attempts": 3,
  "initial_retry_ms": 3000,
  "max_retry_ms": 15000,
  "expires_at": "2026-07-10T10:05:00+08:00",
  "offline_behavior": "fallback_to_lark"
}
```

不同业务使用不同策略：

| 业务 | deadline | 离线策略 |
|---|---|---|
| 会前提醒 | 会议开始前 | 飞书备用通知 |
| 日程晨报 | 配置时间后 30 分钟 | 在线后可补播一次 |
| 外勤出发提醒 | 建议出发时间前 | 飞书备用通知 |
| 出差提醒 | 当日晚间窗口 | 在线后补播或飞书 |
| 普通聊天回复 | 当前 turn 有效期 | 过期后取消 |
| 健康提醒 | 10 分钟 | 过期丢弃 |

### 15.5 API

OpenClaw 提交展示计划：

```http
POST /v3/deliveries
```

查询交付状态：

```http
GET /v3/deliveries/{delivery_id}
```

取消：

```http
POST /v3/deliveries/{delivery_id}/cancel
```

设备大数据接口：

```text
POST /v3/tts
POST /v3/vision/captures
GET  /v3/ota/manifest
GET  /v3/ota/images/{version}
```

旧 `/command` 接口在迁移期保留。它只负责转换并写入新的 Command Store，不再直接进入内存队列。

---

## 16. OpenClaw V3

### 16.1 业务能力

保留以下领域能力：

- `agenda_briefing`
- `calendar_assistant`
- `meeting_reminder_and_notify`
- `travel_planner`
- `wellbeing_companion`

统一输入：

```json
{
  "event_id": "evt_xxx",
  "type": "meeting_starting_soon",
  "timestamp": "2026-07-10T10:55:00+08:00",
  "user_id": "user_xxx",
  "payload": {},
  "context": {
    "device_id": "xiaopai_xxx",
    "timezone": "Asia/Shanghai"
  }
}
```

统一输出：

```json
{
  "speech": "五分钟后项目会议将在中一会议室开始。",
  "presentation": {
    "emotion": "wink",
    "motion": "look_at_user",
    "light": "blink"
  },
  "actions": [],
  "follow_up": {
    "expected": false
  },
  "context_patch": {
    "current_focus": {
      "type": "calendar_event",
      "event_id": "event_xxx"
    }
  },
  "delivery_policy": {
    "presence_requirement": "preferred",
    "offline_behavior": "fallback_to_lark",
    "expires_at": "2026-07-10T11:00:00+08:00"
  }
}
```

### 16.2 确定性展示交付

主动调度链路使用确定性路径：

```text
Scheduler
-> assistant.handleEvent
-> StructuredResponse
-> Render Plan Adapter
-> Xiaopai Delivery API
-> persistent delivery
-> device rendered ACK
```

调度器不再把一个已经生成好的 `StructuredResponse` 再包装成自然语言提示词，交给第二次 agent turn 决定是否调用机器人。

对话场景仍允许 agent 主动调用 `xiaopaiControl.execute`。该调用只表示提交展示计划，不把 `queued` 当作用户已收到。

### 16.3 Scheduler 状态

```text
detected
-> generated
-> submitted
-> delivered
-> failed / expired / cancelled
```

- `generated`：业务响应已经生成。
- `submitted`：展示计划已经持久化到 Xiaopai Server。
- `delivered`：设备返回 `rendered`。
- `failed`：达到最大重试次数。
- `expired`：超过业务有效期。

Scheduler 扫描线程不阻塞等待设备。Delivery Coordinator 异步更新最终结果。

### 16.4 幂等

OpenClaw 使用持久化 `IdempotencyStore`。

至少保存：

- `event_id`。
- side effect 类型。
- 输入摘要。
- 外部 resource id。
- 结果。
- 创建时间。
- 失效时间。

必须覆盖：

- 飞书日程创建。
- 飞书消息发送。
- 主动提醒展示计划。
- 迟到通知。
- 重复 scheduler 扫描。
- 进程重启后的重试。

进程内 `MemoryIdempotencyStore` 只用于测试。

### 16.5 外部工具边界

保留适配器接口：

- `LarkCalendarAdapter`
- `LarkContactAdapter`
- `LarkIMAdapter`
- `WeatherAdapter`
- `RouteAdapter`
- `UserProfileAdapter`
- `ContextStore`
- `IdempotencyStore`
- `XiaopaiDeliveryAdapter`

业务 handler 不依赖 `lark-cli`、OpenAPI 或具体网络实现。

---

## 17. 用户流程

### 17.1 主动语音交互

```text
本地唤醒或触摸
-> Active / Recording
-> 固定 source lease
-> pre-roll + utterance 上传
-> ASR final
-> WaitingReply
-> OpenClaw 业务处理
-> Render Plan 持久化
-> speak 入队
-> Speaking
-> rendered ACK
-> echo guard
-> Monitoring
```

### 17.2 创建飞书日程

```text
用户语音
-> ASR
-> calendar_assistant
-> 参数校验
-> 联系人解析
-> 歧义时追问
-> 持久幂等记录
-> 创建日程
-> 保存 resource_id
-> 机器人播报结果
```

写操作只在必要参数明确后执行。

### 17.3 会前提醒和迟到通知

```text
Scheduler 发现即将开始的会议
-> meeting_reminder
-> 保存 current_focus
-> 提交提醒
-> find_owner best effort
-> 设备播报或飞书备用通知
-> 用户说晚五分钟
-> 从 current_focus 获取会议
-> 解析通知目标
-> 幂等发送飞书消息
-> 播报发送结果
```

### 17.4 主动提醒

```text
Scheduler 检测业务事件
-> 生成 StructuredResponse
-> 保存 delivery
-> 选择在线绑定设备
-> 设备执行
-> rendered ACK
-> delivery 标记 delivered
```

设备离线时根据业务策略：

- 等待重连。
- 转飞书通知。
- 过期丢弃。
- 记录失败供诊断。

### 17.5 寻找用户

```text
presentation.motion = look_at_user
-> Supervisor 创建限时 VisionJob
-> 多角度拍照
-> server 检测人脸
-> motion 调整
-> 返回 found 或 not_found
-> 根据 presence_requirement 决定播报或备用通道
```

### 17.6 Quiet 唤醒

```text
Quiet / Monitoring
-> 本地唤醒词
-> Active
-> 本地确认音
-> 开始正常录音
```

server 不在线时仍能完成唤醒、状态切换和本地提示。业务请求在网络恢复后重试或提示离线。

---

## 18. 健康提醒

### 18.1 感知数据

设备只负责定时采集。服务端 `posture_service` 负责推理。

标准观察结果：

```json
{
  "observation_id": "obs_xxx",
  "device_id": "xiaopai_xxx",
  "timestamp": "2026-07-10T15:00:00+08:00",
  "person_present": true,
  "posture": "sitting",
  "posture_confidence": 0.84,
  "motion_score": 0.08,
  "chair_roi_match": true
}
```

### 18.2 久坐状态

真正的 `sedentary_detected` 需要：

1. 固定机位和 chair ROI 标定。
2. 人体或上半身姿态模型。
3. 在 30 分钟窗口内，大部分有效观察为 sitting。
4. 没有超过允许时长的离开。
5. 没有连续 standing 或明显活动。
6. 置信度达到配置阈值。
7. 当前不在会议、通话或提醒冷却期。

建议默认：

```text
采样间隔：2 分钟
观察窗口：30 分钟
有效 sitting 占比：>= 80%
姿态置信度：>= 0.75
离开宽限：5 分钟
连续 standing reset：2 次
提醒冷却：60 分钟
```

图片默认不长期保存。只保存派生观察结果和必要诊断采样。

### 18.3 降级能力

姿态模型或机位标定不可用时，只产生：

```text
continuous_presence_detected
```

该事件只能触发“连续在场或专注提醒”，不能声明用户已经久坐。

---

## 19. 安全、隐私和 OTA

### 19.1 凭据

- Wi-Fi、device token 和用户配置只写入 NVS。
- 源码和 sdkconfig 不保存真实凭据。
- 每台设备拥有独立 token。
- provisioning 使用一次性会话或受限本地入口。
- 生产环境启用 NVS 加密或等效安全存储。

### 19.2 传输安全

- Control WebSocket 使用 WSS。
- Realtime Audio 使用 WSS。
- Bulk HTTP 使用 HTTPS。
- 设备校验 server certificate。
- 服务端校验 device token。
- 重要控制消息带 sequence 和 HMAC 或完整 TLS 会话保护。
- server 只监听受控网络接口。

### 19.3 隐私

- Quiet 模式普通语音不上传。
- 只有本地唤醒后才启动云端 ASR。
- 图片用于单次视觉任务。
- 默认不保存原始图片。
- 诊断图片需要显式开关和自动过期。
- 健康提醒只保存结构化姿态元数据。
- 日志不记录完整语音文本、token 和用户敏感字段。

### 19.4 OTA

OTA 流程：

```text
检查 manifest
-> 校验目标硬件和最低版本
-> 下载到非活动分区
-> 校验 hash 和签名
-> 设置待启动分区
-> 重启
-> 自检
-> 标记 app valid
-> 失败自动 rollback
```

自检至少包括：

- NVS 可读。
- Supervisor 启动。
- 内置音频可用。
- 控制网络可以启动。
- heap 达到最低要求。
- 关键任务没有 watchdog 或 stack overflow。

OTA 期间系统进入 `Maintenance`，停止普通语音、视觉和动作任务。

---

## 20. 故障处理

| 故障 | 处理 |
|---|---|
| Control WebSocket 断开 | 指数退避重连。保留终态 ACK |
| Realtime Audio 断开 | 结束当前 ASR session。回到 Monitoring |
| DJI 拔出 | 当前 utterance `source_lost`。切回内置麦克风 |
| DJI 枚举失败 | 内置麦克风继续工作。后台限次重试 |
| TTS 超时 | 中止当前 speech。返回 failed。可播放本地错误音 |
| Camera 失败 | 进入 Maintenance 重建一次。再次失败禁用视觉 |
| I2C 错误 | 统一维护恢复。禁止模块自行重建 |
| Servo 无响应 | 停止动作。记录故障。限制后续 motion |
| 内部 SRAM 低于阈值 | 停止低优先级任务。上传诊断。必要时安全重启 |
| PSRAM 最大块持续下降 | 停止视觉和长录音。记录碎片诊断 |
| brownout 重启 | 增加计数。降低负载。连续发生时禁用 USB 外设 |
| server 重启 | 从 SQLite 恢复命令和 delivery |
| OpenClaw 重启 | 从持久幂等和 scheduler 状态恢复 |
| 设备重启 | 使用 boot_id、lease 和 NVS 去重恢复 |

安全重启前必须尽量发送或保存最后的 Fault 记录。

---

## 21. 实施顺序

### 阶段 1：固件状态和资源收敛

1. 建立 `supervisor_task`。
2. 删除重复状态变量的写入口。
3. 合并表情、触摸和灯带到 `ui_task`。
4. 建立固定队列和音频块池。
5. 加入 stack watermark、heap 和 queue 指标。
6. 将应用常驻任务栈控制在 72 KiB 内。

### 阶段 2：控制和命令可靠性

1. 新建 SQLite Command Store。
2. 建立 Control WebSocket。
3. 建立命令 lease、续租、ACK 和重投。
4. 加入 boot_id 和两级去重。
5. 旧 `/command` 转换到新 Command Store。
6. 将 sequence 限制为扁平 8 步 ActionPlan。

### 阶段 3：音频输入和输出

1. 内置麦克风开机立即可用。
2. 建立 source lease 和 source generation。
3. DJI 增加身份和稳定性判定。
4. 增加 FIR 3:1 decimator。
5. 分离每源 gain 和 VAD profile。
6. 实时 ASR 接入统一 pre-roll。
7. 建立 SpeechQueue 和 turn cancellation。
8. 增加 400 ms echo guard。

### 阶段 4：camera、I2C、motion 和 power

1. 建立 `BoardI2cService`。
2. camera 改为持久化生命周期。
3. 图像改为直接分块上传。
4. 修正舵机标定和安全范围。
5. 增加 servo ping、ACK 和反馈。
6. 加入 PowerManager 和 brownout 策略。
7. 完成组合负载电源测试。

### 阶段 5：OpenClaw 真实交付闭环

1. Scheduler 改为确定性 Render Plan 提交。
2. 建立 Delivery Coordinator。
3. `rendered` 作为主动提醒送达终点。
4. `MemoryIdempotencyStore` 替换为持久化实现。
5. 外部工具写操作全部使用幂等键。
6. 增加离线、过期和备用通道策略。

### 阶段 6：本地唤醒和健康感知

1. 接入 WakeNet 或关键词模型。
2. Quiet 模式关闭普通音频上传。
3. 完成自定义唤醒词验收。
4. 接入 server 侧 posture model。
5. 完成 chair ROI 标定。
6. 区分 sedentary 和 continuous presence 事件。

### 阶段 7：安全和量产准备

1. 删除硬编码凭据。
2. 接入 WSS、HTTPS 和设备认证。
3. 增加签名 OTA 和 rollback。
4. 增加 coredump 和诊断导出。
5. 完成 24 小时、7 天和故障注入测试。

---

## 22. 验收测试

### 22.1 状态和并发

1. Recording 中连续到达 3 条 speak。录音不中断，语音按策略排队。
2. Speaking 中到达普通 speak。当前播放完整结束。
3. Speaking 中触发本地 stop。200 ms 级别开始停止。
4. Job 被取消后晚到的完成事件不能改变当前状态。
5. ActionPlan 每一步都能响应取消。
6. 队列满时，所有丢弃或合并命令都有终态 ACK。

### 22.2 音频

1. DJI 在录音中插入。当前 utterance 不混入 DJI PCM。
2. DJI 在录音中拔出。当前 utterance 返回 `source_lost`。
3. DJI 身份不匹配时不能抢占内置麦克风。
4. 48 kHz 到 16 kHz 通过频谱和 ASR 测试。
5. 内置和 DJI 分别完成增益和 VAD 标定。
6. 短唤醒词首字不被截断。
7. 播放结束后不会被自身回声立即重新触发。
8. server 断开时 Quiet 仍可本地唤醒。

### 22.3 命令可靠性

1. 命令收到后设备立即重启。server lease 超时后重投。
2. 非幂等命令重投时设备不重复执行。
3. server 重启后未完成命令仍存在。
4. Control WebSocket 重连后 ACK 可以重放。
5. 长 TTS 正常续租，不产生重复播放。
6. 过期提醒不会在很久以后补播。
7. 多设备环境不会投递到错误设备。

### 22.4 Camera 和 motion

1. camera 连续拍摄 1000 张，无持续 heap 下降。
2. vision 任务中 stop 可以在一轮边界生效。
3. 找不到人脸时返回 `not_found`，不播放成功话术。
4. 时间敏感提醒在 find_owner 失败后按 policy 继续。
5. 舵机超范围命令在入队前拒绝。
6. 软件角度、raw 目标和反馈角度一致。
7. servo 断线后进入明确 Fault。

### 22.5 内存和性能

1. 最坏组合场景最低内部空闲内存不少于 64 KiB。
2. 最大内部连续块不少于 48 KiB。
3. 最大 DMA 连续块不少于 32 KiB。
4. 最大 PSRAM 连续块不少于 512 KiB。
5. 所有任务剩余栈不少于 1 KiB 和 20%。
6. 音频热路径动态分配为零。
7. 图像上传期间 Control WebSocket stop 延迟符合要求。
8. ASR 音频上传期间 UI 和动作不影响音频连续性。

### 22.6 电源

1. DJI 加最大音量不会 brownout。
2. DJI 加双舵机启动不会 brownout。
3. DJI、TTS、camera upload 和舵机组合运行不会重启。
4. 低电量场景能够正确限制负载。
5. USB 热插拔不会造成设备复位。
6. 连续 brownout 策略能够回退内置麦克风。

### 22.7 OpenClaw 和业务交付

1. Scheduler 生成提醒后，只有设备 `rendered` 才标记 delivered。
2. 设备离线时按业务配置走飞书备用通知。
3. 飞书日程创建在进程重启后不会重复。
4. 迟到通知在重复事件下只发送一次。
5. 当前会议上下文可以支持省略对象的二次指令。
6. 路线或天气失败时不编造结果。
7. 姿态模型不可用时不输出 `sedentary_detected`。
8. 健康提醒遵守会议、通话和 cooldown 策略。

### 22.8 OTA 和安全

1. 错误签名固件被拒绝。
2. OTA 下载中断后仍能启动旧版本。
3. 新版本自检失败后自动 rollback。
4. token 和 Wi-Fi 密码不出现在源码和日志。
5. Quiet 模式普通语音不上传。
6. 图片默认不被持久保存。
7. 设备认证失败时不能接收命令。

### 22.9 长时间运行

1. 24 小时连续运行，无崩溃和明显资源下降。
2. 7 天连续运行，无持续 heap 或 PSRAM 最大块下降。
3. 7 天内反复执行语音、拍照、动作、USB 插拔和网络重连。
4. watchdog、stack overflow、brownout 和 coredump 统计为零或符合故障注入预期。

---

## 23. 发布门槛

### 23.1 Demo 版本

必须完成：

- Supervisor。
- 基础命令队列。
- 内置和 DJI 稳定切换。
- 语音对话。
- 飞书日程和通知。
- 基础 camera 和 motion。
- 8 小时运行测试。

### 23.2 办公试点版本

必须完成：

- 持久 Command Store。
- Control WebSocket。
- lease、ACK、重试和去重。
- 主动提醒真实 `rendered` 闭环。
- 24 小时运行。
- 组合负载电源测试。
- 本地 Quiet 唤醒。
- 安全凭据管理。

### 23.3 稳定产品版本

必须完成：

- 7 天连续运行。
- 签名 OTA 和 rollback。
- 完整故障恢复。
- WSS 和 HTTPS。
- 持久幂等。
- 多设备绑定。
- 姿态模型和健康提醒语义校验。
- 量产电源和舵机标定流程。
- 自动化硬件回归测试。

---

## 24. 最终交付结构

建议仓库调整为：

```text
repo/
  firmware/
    main/
      supervisor/
      audio/
      speech/
      vision/
      motion/
      ui/
      board/
      transport/
      health/
    partitions.csv
    sdkconfig.defaults

  xiaopai-server/
    src/
      control_gateway.py
      audio_gateway.py
      command_store.py
      delivery_coordinator.py
      asr_service.py
      tts_service.py
      vision_service.py
      posture_service.py
      ota_service.py
    migrations/
    tests/

  openclaw-skills/
    plugins/
      work-assistant/
      xiaopai-control/
      weather-provider/
    shared/
      contracts/
      idempotency/
      delivery/

  docs/
    architecture_v3.md
    protocol_v3.md
    hardware_test_plan.md
    deployment.md
```

协议定义由 `shared/contracts` 或独立 schema 包统一维护。固件、server 和 OpenClaw 的命令枚举、状态枚举和版本号必须由同一份 schema 生成或校验。
