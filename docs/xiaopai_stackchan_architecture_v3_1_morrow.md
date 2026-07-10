# Xiaopai / Stack-chan V3.1 总体架构与落地方案

版本：V3.1  
日期：2026-07-10  
适用范围：CoreS3 机器人固件、Xiaopai Server、Morrow Agent、飞书及外部服务适配器

---

## 0. 方案目标

本方案用于实现一个可长期在线运行的桌面工作助理机器人。系统需要同时支持语音交互、主动提醒、飞书业务操作、低频视觉感知、头部动作、表情展示和安全 OTA。

最终用户能力包括：

1. 用户通过本地唤醒词或触摸进入语音交互。
2. 机器人完成实时 ASR、Morrow Agent 业务理解、工具调用和 TTS 回复。
3. Morrow 查询和创建飞书日程。
4. Morrow 发送会议迟到通知。
5. Morrow 主动发起日程、会议、外勤、出差和健康提醒。
6. 机器人低频寻找用户并朝向用户。
7. 系统根据在场和姿态信息进行健康提醒。
8. DJI USB 麦克风和内置麦克风可以稳定切换。
9. 网络、USB、摄像头、Morrow 或服务端异常后可以自动恢复。
10. 主动提醒具备持久化、重试、去重和真实送达确认。
11. Morrow 与 Xiaopai Server 之间只通过一条持久双向流式对话连接通信。
12. 固件支持签名 OTA、启动确认和回滚。
13. 设备可以完成至少 7 天连续运行测试。

本方案不将以下能力列为 V3.1 必须目标：

1. 高频实时视觉跟踪。
2. 本地大模型推理。
3. 无 AEC 条件下的全双工语音打断。
4. 仅凭人脸存在判断用户久坐。
5. 在同一个 USB-C 口同时进行 USB Host、USB Device 调试和设备供电。
6. Morrow 与 Xiaopai Server 之间使用 REST、RPC、回调、共享数据库或第二条消息通道。

---

## 1. 系统分层

系统分为三层。

```text
Morrow Agent 业务层
  对话、日程、会议、出行、天气、健康关怀、记忆、调度、工具调用
                           ||
                 Morrow Dialogue Stream
             单一持久连接，双向流式传递
                           ||
Xiaopai Server 控制与交付层
  ASR、对话桥接、设备注册、命令持久化、ACK、TTS、视觉、OTA
                           |
                           v
CoreS3 设备执行层
  本地唤醒、录音、播放、摄像头、舵机、表情、触摸、健康监控
```

### 1.1 Morrow Agent 业务层职责

Morrow 负责：

- 理解用户输入和系统事件。
- 维护多轮对话上下文和当前业务焦点。
- 查询飞书日历、通讯录和消息。
- 创建日程和邀请参会人。
- 发送飞书通知。
- 查询天气和路线。
- 生成日程复盘、会议提醒、出行建议和健康关怀内容。
- 运行主动提醒调度器。
- 执行外部工具并维护写操作幂等。
- 通过同一条流式对话连接接收用户消息和设备结果。
- 通过同一条流式对话连接输出文字、展示意图、交付策略和主动提醒。

Morrow 不直接连接设备，不直接操作摄像头、舵机、扬声器和屏幕。

### 1.2 Xiaopai Server 职责

Xiaopai Server 负责：

- 维护与 Morrow 的唯一双向流式对话连接。
- 将 ASR 结果和系统事件流式发送给 Morrow。
- 接收 Morrow 的流式回复、主动消息和展示意图。
- 组装、校验并持久化 Morrow 已提交的输出片段。
- 把已提交的输出转换成设备原子命令。
- 保存设备、对话、命令和交付状态。
- 维护设备 Control WebSocket。
- 维护设备 Realtime Audio WebSocket。
- 提供设备侧 TTS、图像和 OTA 大数据 HTTP 服务。
- 管理命令租约、续租、重试、去重和超时。
- 将设备 ACK、视觉结果和交付结果通过同一条对话流返回 Morrow。
- 执行 ASR、TTS、视觉和姿态模型。

Xiaopai Server 不通过其他 API 调用 Morrow，也不与 Morrow 共享数据库。

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
| 设备控制连接 | `control_transport_task` |
| 设备实时 ASR 连接 | `realtime_audio_task` |
| 系统状态 | `supervisor_task` |

USB Host 组件保留其内部任务。USB 回调只写入固定环形缓冲，不执行网络、ASR 或业务逻辑。

### 2.3 固定消息和固定缓冲

任务间使用固定大小 POD 消息。

音频热路径使用固定块池和环形缓冲。禁止在每个音频帧中反复调用 `malloc`、`free` 或创建 `std::vector`。

摄像头上传直接读取 framebuffer。禁止为每张图再复制完整图像。

### 2.4 设备链路按实时性分离

设备与 Xiaopai Server 使用三类链路：

```text
Control WebSocket
  命令、ACK、心跳、状态、取消、租约续期

Realtime Audio WebSocket
  Opus 或 PCM 音频帧、ASR 会话状态、识别结果

Bulk HTTP
  TTS 音频、图像上传、OTA、诊断文件
```

图片上传、TTS 下载和 OTA 不得阻塞设备控制 WebSocket。

### 2.5 Morrow 与 Server 只使用一条流式对话连接

Morrow 与 Xiaopai Server 之间只存在一个逻辑通信通道：

```text
Morrow Dialogue Stream
```

该通道使用持久、全双工、可恢复的流式连接。默认传输为 WSS。

以下内容全部通过该流传递：

- 用户文本增量和最终文本。
- 系统事件和主动提醒上下文。
- Morrow 文字增量和已提交语句。
- 表情、动作、灯光和交付策略。
- 取消事件。
- 设备执行结果。
- `rendered`、`failed`、`expired` 等交付状态。
- 心跳、ACK、流恢复和背压信息。

禁止在 Morrow 与 Xiaopai Server 之间增加：

- REST API。
- 独立 RPC。
- Webhook。
- 回调端口。
- 第二条 WebSocket。
- 消息队列桥接。
- 共享数据库。
- 文件轮询。

一个 Morrow 实例只保持一条活动连接。多个用户、设备和对话通过 `conversation_id` 在该连接内复用。

### 2.6 持久化后再投递

Morrow 已提交的输出片段和设备命令必须先写入持久化存储，再发送给设备。

“正在生成”“片段已提交”“设备已收到”和“用户可感知播放完成”是不同状态。

未提交的文字增量不能触发 TTS、动作或外部交付。

### 2.7 普通任务不抢占，安全事件可以取消

普通语音、表情、动作和视觉任务按准入策略排队。

以下事件可以取消当前作业：

- 用户本地停止。
- 远程安全停止。
- USB 或音频源丢失。
- 硬件故障。
- OTA 维护切换。
- Supervisor 超时。
- 当前会话被新的用户会话替换。
- Morrow 或 Server 明确发送当前 turn 取消。

---

## 3. 总体模块图

```text
                         Morrow Agent
       Dialogue Runtime / Scheduler / Memory / Tool Adapters
             |                  |                 |
       飞书日历与 IM          天气与路线        幂等与上下文
             \                  |                 /
                    Morrow Dialogue Stream
           单一 WSS，全双工，增量、提交、ACK、恢复
                              ||
                              ||
                    Xiaopai Server
       morrow_stream_gateway / dialogue_store / turn_assembler
                              |
                  render_and_delivery_coordinator
                              |
                    Persistent Command Store
                              |
        +---------------------+----------------------+
        |                     |                      |
 Device Control WS     Realtime Audio WS        Bulk HTTP
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

Morrow 与 Xiaopai Server 之间没有独立 Delivery API。主动消息由 Morrow 在同一条对话流中发起。设备执行结果由 Server 在同一条流中返回。

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
- `WaitingReply`：ASR 完成，等待 Morrow 在流式对话中返回已提交回复。
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

## 15. Xiaopai Server V3.1

### 15.1 模块结构

```text
src/
  app.py
  device_registry.py
  control_gateway.py
  audio_gateway.py
  morrow_stream_gateway.py
  dialogue_store.py
  turn_assembler.py
  render_coordinator.py
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

职责划分：

- `morrow_stream_gateway.py`：唯一的 Morrow 流式对话连接、握手、ACK、恢复和背压。
- `dialogue_store.py`：保存流帧、conversation、turn、提交片段和取消状态。
- `turn_assembler.py`：将增量帧组装成可提交的文字、展示和交付策略。
- `render_coordinator.py`：把已提交内容转换成设备语音、表情、动作和灯光命令。
- `delivery_coordinator.py`：跟踪设备交付，并把结果写回 Morrow Dialogue Stream。
- `command_store.py`：设备命令持久化、lease、retry 和 ACK。
- `audio_gateway.py`：设备音频输入和 ASR。
- `control_gateway.py`：设备控制连接。

### 15.2 持久化

使用 SQLite WAL。

核心表：

```text
devices
device_sessions
agent_stream_sessions
dialogue_frames
conversations
conversation_turns
committed_segments
deliveries
commands
command_attempts
command_acks
captures
ota_releases
```

持久化规则：

1. Morrow 帧先写入 `dialogue_frames`，再返回流 ACK。
2. 只有 `segment.commit` 或 `turn.commit` 可以生成可交付内容。
3. 已提交内容先写入 `committed_segments` 和 `deliveries`。
4. 设备命令先写入 `commands`，再投递给设备。
5. 设备终态 ACK 先写入数据库，再通过 Morrow Dialogue Stream 返回。
6. 未提交 delta 可按容量限制短期保存，不参与设备执行。
7. stream、turn、segment、delivery 和 command 都使用独立幂等 ID。

每次设备连接创建 `device_session`，包含 `boot_id`、固件版本、能力和在线状态。

每次 Morrow 连接创建或恢复 `agent_stream_session`，包含：

- `agent_instance_id`
- `stream_id`
- `connection_epoch`
- `last_rx_seq`
- `last_tx_seq`
- `last_rx_ack`
- `last_tx_ack`
- `connected_at`
- `disconnected_at`

### 15.3 Morrow 流处理

Xiaopai Server 对 Morrow 只暴露一个持久流式对话入口。

Server 在该流中发送：

- 用户 ASR 文本增量。
- 用户文本提交。
- 本地触摸和唤醒事件。
- 健康、设备和业务系统事件。
- 设备执行结果。
- 交付状态。
- 取消事件。
- 流级 ACK 和恢复状态。

Server 在该流中接收：

- Morrow assistant turn。
- 文字 delta。
- 稳定语句 commit。
- 表情、动作和灯光 commit。
- delivery policy commit。
- 主动提醒 turn。
- turn commit 或 cancel。
- 流级 ACK 和心跳。

Server 不解析 Morrow 的自然语言来猜测机械动作。动作、表情和交付策略必须由同一 assistant turn 内的结构化 content block 明确提交。

默认行为是把已提交文字转换成 speak。结构化展示块是可选增强。

### 15.4 设备选择

每个 `conversation_id` 和 Morrow user 显式绑定 `device_id`。

只有在以下条件同时满足时才允许自动选择设备：

1. 当前用户只绑定一个设备。
2. 该设备 Control WebSocket 在线。
3. 最近心跳在 TTL 内。
4. 设备能力满足当前展示计划。

不得回退到历史默认设备或其他用户设备。

主动 turn 未指定设备时，Server 根据 `user_id` 绑定关系选择。选择结果通过同一流返回 Morrow。

### 15.5 重试策略

设备命令级策略包含：

```json
{
  "max_attempts": 3,
  "initial_retry_ms": 3000,
  "max_retry_ms": 15000,
  "expires_at": "2026-07-10T10:05:00+08:00",
  "offline_behavior": "report_to_morrow"
}
```

不同业务使用不同策略：

| 业务 | deadline | 设备离线行为 |
|---|---|---|
| 会前提醒 | 会议开始前 | 返回 Morrow，由 Morrow 决定飞书备用通知 |
| 日程晨报 | 配置时间后 30 分钟 | 在线后可补播一次 |
| 外勤出发提醒 | 建议出发时间前 | 返回 Morrow，由 Morrow 决定备用通道 |
| 出差提醒 | 当日晚间窗口 | 在线后补播或返回 Morrow |
| 普通聊天回复 | 当前 turn 有效期 | 过期后取消并提示离线 |
| 健康提醒 | 10 分钟 | 过期丢弃并回报 Morrow |

Server 不直接调用 Morrow 工具。设备离线、交付失败或过期后，Server 只在同一流中发送状态。是否调用飞书等备用工具由 Morrow 决定。

### 15.6 对外接口边界

Morrow 与 Server 之间没有 HTTP API。

唯一 Agent 边界是：

```text
Morrow Dialogue Stream
```

设备侧继续保留：

```text
Control WebSocket
Realtime Audio WebSocket
POST /v3/tts
POST /v3/vision/captures
GET  /v3/ota/manifest
GET  /v3/ota/images/{version}
```

管理和诊断接口可以独立存在，但不能被 Morrow 用于对话、交付、状态查询或取消。

旧 `/command` 接口只用于人工调试和迁移。Morrow 不得调用该接口。

---

## 16. Morrow Agent 与流式对话协议

### 16.1 Morrow 业务能力

Morrow 保留以下领域能力：

- `agenda_briefing`
- `calendar_assistant`
- `meeting_reminder_and_notify`
- `travel_planner`
- `wellbeing_companion`

Morrow 内部负责：

- 对话状态和长期、短期记忆。
- 主动提醒 scheduler。
- 飞书日历、联系人和 IM 工具。
- 天气和路线工具。
- 用户配置。
- 外部写操作幂等。
- 业务事件去重。
- 对话生成和结构化展示意图。

Morrow 不包含 Xiaopai 设备驱动，也不直接访问 Xiaopai Server 数据库。

### 16.2 单一流连接

一个 Morrow Agent 实例与 Xiaopai Server 之间只建立一条持久全双工连接。

默认形式：

```text
WSS /morrow/dialogue/stream
```

一个物理连接可以通过 `conversation_id` 复用多个用户和设备对话。

流具备：

- 双向增量传输。
- 双向累积 ACK。
- 帧级 sequence。
- 断线恢复。
- 未确认帧重放。
- 背压窗口。
- 心跳和连接 epoch。
- turn 和 segment 幂等。

任何业务都不能绕过该流访问另一套 Agent 接口。

### 16.3 通用帧结构

```json
{
  "protocol_version": 1,
  "stream_id": "stream_xxx",
  "connection_epoch": 3,
  "seq": 1025,
  "ack": 992,
  "frame_id": "frame_xxx",
  "conversation_id": "conv_xxx",
  "turn_id": "turn_xxx",
  "type": "assistant.text.segment_commit",
  "timestamp": "2026-07-10T11:00:00+08:00",
  "payload": {}
}
```

字段要求：

- `seq` 在当前发送方向单调递增。
- `ack` 是对相反方向已持久接收的最大连续序号。
- `frame_id` 全局唯一。
- `conversation_id` 标识逻辑对话。
- `turn_id` 标识一次用户或主动交互。
- `connection_epoch` 每次新物理连接递增。
- 重放帧保留原 `frame_id` 和原 `seq`。
- 接收方按 `frame_id` 去重。

### 16.4 握手和恢复

连接建立后，发起方发送：

```json
{
  "type": "stream.hello",
  "payload": {
    "agent_instance_id": "morrow_xxx",
    "protocol_version": 1,
    "last_received_seq": 992,
    "last_sent_seq": 1024,
    "resume_token": "resume_xxx",
    "capabilities": [
      "text_stream",
      "proactive_turn",
      "presentation_block",
      "delivery_feedback",
      "turn_cancel"
    ]
  }
}
```

对端返回：

```json
{
  "type": "stream.hello_ack",
  "payload": {
    "stream_id": "stream_xxx",
    "resume_accepted": true,
    "peer_last_received_seq": 1024,
    "heartbeat_interval_ms": 5000,
    "max_inflight_frames": 32
  }
}
```

恢复规则：

1. 双方从对端最后确认序号之后重放。
2. 已确认帧不重放。
3. 重放不会重复执行工具、TTS 或动作。
4. 未提交 delta 可以重放，也可以由发送方放弃并重新开始 turn。
5. 已提交 segment 必须保留相同 `segment_id`。
6. resume 失败时创建新 `stream_id`，但 conversation 和 turn 幂等关系保持不变。

### 16.5 Server 到 Morrow 的帧

主要类型：

```text
user.turn.start
user.text.delta
user.text.commit
system.event
device.event
device.action_result
delivery.status
turn.cancel
stream.ack
stream.ping
stream.pong
```

用户输入示例：

```json
{
  "type": "user.text.commit",
  "conversation_id": "conv_xxx",
  "turn_id": "turn_user_001",
  "payload": {
    "text": "帮我约明天下午三点和张三开会",
    "asr_confidence": 0.94,
    "language": "zh-CN",
    "device_id": "xiaopai_xxx"
  }
}
```

ASR partial 使用 `user.text.delta`。Morrow 可以用它降低首响应延迟，但不得在 `user.text.commit` 前执行不可逆工具写操作。

系统事件示例：

```json
{
  "type": "system.event",
  "conversation_id": "conv_xxx",
  "turn_id": "turn_event_001",
  "payload": {
    "event_id": "evt_sedentary_001",
    "event_type": "sedentary_detected",
    "occurred_at": "2026-07-10T15:00:00+08:00",
    "data": {}
  }
}
```

设备和交付结果也通过相同流返回，不使用查询 API。

### 16.6 Morrow 到 Server 的帧

主要类型：

```text
assistant.turn.start
assistant.text.delta
assistant.text.segment_commit
assistant.presentation.commit
assistant.delivery_policy.commit
assistant.turn.commit
assistant.turn.cancel
agent.status
stream.ack
stream.ping
stream.pong
```

文字增量示例：

```json
{
  "type": "assistant.text.delta",
  "conversation_id": "conv_xxx",
  "turn_id": "turn_agent_001",
  "payload": {
    "segment_id": "seg_001",
    "delta": "好的，"
  }
}
```

稳定语句提交示例：

```json
{
  "type": "assistant.text.segment_commit",
  "conversation_id": "conv_xxx",
  "turn_id": "turn_agent_001",
  "payload": {
    "segment_id": "seg_001",
    "text": "好的，我来帮你创建日程。",
    "order": 1
  }
}
```

展示提交示例：

```json
{
  "type": "assistant.presentation.commit",
  "conversation_id": "conv_xxx",
  "turn_id": "turn_agent_001",
  "payload": {
    "presentation_id": "presentation_001",
    "emotion": "wink",
    "motion": "look_at_user",
    "light": "blink",
    "apply_before_segment_id": "seg_001"
  }
}
```

交付策略示例：

```json
{
  "type": "assistant.delivery_policy.commit",
  "conversation_id": "conv_xxx",
  "turn_id": "turn_agent_001",
  "payload": {
    "policy_id": "policy_001",
    "presence_requirement": "preferred",
    "expires_at": "2026-07-10T11:00:00+08:00",
    "on_device_offline": "report_to_morrow"
  }
}
```

### 16.7 增量、提交和执行语义

流式输出使用三层语义：

```text
delta
-> segment_commit
-> turn_commit
```

规则：

1. `delta` 只用于实时展示和缓冲。
2. `delta` 不触发 TTS、动作、飞书通知或其他外部效果。
3. `text.segment_commit` 表示一句稳定文本，可以持久化并进入 SpeechQueue。
4. `presentation.commit` 表示结构化展示可以执行。
5. `delivery_policy.commit` 表示当前 turn 的交付约束已经确定。
6. `turn.commit` 表示 Morrow 不再为该 turn 增加内容。
7. Server 可以在 turn 未结束时播放已经提交的语句。
8. Morrow 不得修改已经提交的 segment。
9. 需要纠正已提交内容时，发送新 segment，而不是覆盖旧 segment。
10. Server 在执行每个 committed segment 前检查 turn cancellation 和 deadline。

该设计保留流式低延迟，同时防止未稳定 token 被朗读或执行。

### 16.8 主动提醒

主动提醒由 Morrow 在同一流中发起。

```text
Morrow Scheduler
-> assistant.turn.start(trigger=proactive)
-> delivery_policy.commit
-> text.segment_commit
-> presentation.commit
-> assistant.turn.commit
```

`assistant.turn.start` 必须包含：

```json
{
  "trigger": "proactive",
  "event_id": "evt_meeting_001",
  "user_id": "user_xxx",
  "preferred_device_id": "xiaopai_xxx",
  "created_at": "2026-07-10T10:55:00+08:00"
}
```

Server 收到后：

1. 按 `event_id` 和 `turn_id` 去重。
2. 创建持久 delivery。
3. 返回 `delivery.status=accepted`。
4. 按 committed segment 逐步生成设备命令。
5. 收到设备 ACK 后返回 `rendered`、`failed`、`expired` 或 `cancelled`。

Morrow Scheduler 只有在收到 `rendered` 后才把机器人交付标记为成功。

设备离线或交付失败时，Server 通过相同流发送状态。Morrow 再决定等待、取消或使用飞书工具发送备用通知。

### 16.9 取消

取消也只走同一流。

Server 发出：

```json
{
  "type": "turn.cancel",
  "conversation_id": "conv_xxx",
  "turn_id": "turn_agent_001",
  "payload": {
    "reason": "user_local_stop"
  }
}
```

Morrow 返回：

```json
{
  "type": "assistant.turn.cancel",
  "conversation_id": "conv_xxx",
  "turn_id": "turn_agent_001",
  "payload": {
    "reason": "cancel_acknowledged"
  }
}
```

用户本地 stop 时，Server 先立即停止设备播放和动作，再向 Morrow 发送取消。不得等待 Morrow ACK 后才停止硬件。

### 16.10 背压和顺序

默认配置：

```text
heartbeat：5 秒
最大未确认帧：32
单帧最大 JSON：64 KiB
单个 delta 最大文本：4 KiB
单 turn 未提交缓存：256 KiB
ACK 刷新：100 ms 或累计 8 帧
```

规则：

- 超出 inflight window 后停止发送新 delta。
- control、cancel、ACK 和 heartbeat 优先于普通 delta。
- 同一 turn 内 committed segment 按 `order` 执行。
- 不同 conversation 可以并发生成。
- 同一 device 同一时刻只允许一个主 Speaking turn。
- 超出缓存限制时，优先丢弃未提交 delta，不丢 committed segment 和终态状态。

### 16.11 幂等和工具边界

Morrow 使用持久化 `IdempotencyStore`。

至少保存：

- `event_id`
- `turn_id`
- 工具 side effect 类型
- 输入摘要
- 外部 resource id
- 结果
- 创建时间
- 失效时间

必须覆盖：

- 飞书日程创建。
- 飞书消息发送。
- 主动提醒 turn。
- 迟到通知。
- 重复 scheduler 扫描。
- 流重放。
- Morrow 进程重启。
- Server 重复发送 `user.text.commit` 或 `system.event`。

Morrow 内部工具适配器包括：

- `LarkCalendarAdapter`
- `LarkContactAdapter`
- `LarkIMAdapter`
- `WeatherAdapter`
- `RouteAdapter`
- `UserProfileAdapter`
- `ContextStore`
- `IdempotencyStore`

Morrow 与 Xiaopai Server 的唯一边界适配器是：

```text
MorrowDialogueStreamAdapter
```

它只维护同一条流，不提供额外请求接口。

---

## 17. 用户流程

### 17.1 主动语音交互

```text
本地唤醒或触摸
-> Active / Recording
-> 固定 source lease
-> pre-roll + utterance 上传到 Xiaopai Server
-> ASR partial 通过 Morrow Dialogue Stream 发送 user.text.delta
-> ASR final 发送 user.text.commit
-> WaitingReply
-> Morrow 处理对话和工具
-> Morrow 流式发送 assistant.text.delta
-> Morrow 提交稳定语句 assistant.text.segment_commit
-> Server 持久化 segment
-> Server 生成 speak 命令
-> Speaking
-> device rendered ACK
-> Server 通过同一流发送 delivery.status
-> echo guard
-> Monitoring
```

Morrow 可以继续生成下一条 committed segment。Server 按顺序播放，不等待整个 turn 结束。

### 17.2 创建飞书日程

```text
用户语音
-> ASR user.text.commit
-> Morrow calendar_assistant
-> 参数校验
-> 联系人解析
-> 歧义时流式返回追问
-> 用户下一 turn 回答
-> Morrow 持久幂等记录
-> 创建飞书日程
-> 保存 resource_id
-> Morrow 提交结果语句
-> Server 播放并返回 rendered
```

写操作只在必要参数明确且用户 turn 已 commit 后执行。

### 17.3 会前提醒和迟到通知

```text
Morrow Scheduler 发现即将开始的会议
-> 在同一流发起 proactive assistant turn
-> Server 持久化 delivery
-> find_owner best effort
-> 设备播报
-> Server 返回 rendered
-> 用户说晚五分钟
-> 新 user turn 通过同一流进入 Morrow
-> Morrow 从 current_focus 获取会议
-> 幂等发送飞书消息
-> Morrow 提交发送结果语句
-> 设备播报
```

设备离线时，Server 在同一流返回 `device_offline`。Morrow 决定是否发送飞书备用通知。

### 17.4 主动提醒

```text
Morrow Scheduler 检测业务事件
-> 创建稳定 event_id
-> 在同一流发送 assistant.turn.start(trigger=proactive)
-> 流式提交文字、展示和交付策略
-> Server 保存 delivery
-> Server 选择绑定设备
-> 设备执行
-> rendered ACK
-> Server 在同一流返回 delivery.status=rendered
-> Morrow Scheduler 标记 delivered
```

连接断开时，Morrow 保留未确认主动 turn。恢复连接后按原 `event_id` 和 `turn_id` 重放。

### 17.5 寻找用户

```text
Morrow 提交 presentation.motion = look_at_user
-> Server 创建限时 VisionJob
-> 多角度拍照
-> Server 检测人脸
-> motion 调整
-> 返回 found 或 not_found
-> Server 在同一流发送 device.action_result
-> Morrow 根据结果和 presence policy 继续生成内容
```

时间敏感提醒不等待无限期视觉结果。达到视觉 deadline 后按 delivery policy 继续。

### 17.6 Quiet 唤醒

```text
Quiet / Monitoring
-> 本地唤醒词
-> Active
-> 本地确认音
-> 开始正常录音
-> ASR 后通过 Morrow Dialogue Stream 发起 user turn
```

Morrow 或 Server 不在线时，设备仍能完成本地唤醒、状态切换和离线提示。未提交用户 turn 不在很久以后自动重放，避免产生过期回复。

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
- Morrow Agent 和 Xiaopai Server 拥有独立服务身份。
- provisioning 使用一次性会话或受限本地入口。
- 生产环境启用 NVS 加密或等效安全存储。

### 19.2 传输安全

- 设备 Control WebSocket 使用 WSS。
- 设备 Realtime Audio 使用 WSS。
- 设备 Bulk HTTP 使用 HTTPS。
- Morrow Dialogue Stream 使用 WSS。
- Morrow 与 Server 使用双向身份认证或短期服务 token。
- 设备校验 Server certificate。
- Server 校验 device token。
- Morrow 和 Server 校验对端身份、`stream_id`、`connection_epoch` 和 sequence。
- 重要控制消息依赖 TLS 完整性，并保留帧级 sequence 和去重。
- Server 只监听受控网络接口。
- Morrow 和 Server 不共享数据库凭据。

### 19.3 隐私

- Quiet 模式普通语音不上传。
- 只有本地唤醒后才启动云端 ASR。
- Xiaopai Server 默认只向 Morrow 发送 ASR 文本，不发送原始音频。
- 图片用于单次视觉任务。
- 默认不保存原始图片。
- 诊断图片需要显式开关和自动过期。
- 健康提醒只保存结构化姿态元数据。
- 流日志不记录 token、完整凭据和敏感工具参数。
- 未提交文本 delta 按短期缓冲处理，并设置自动清理期限。
- 对话和工具日志按用户数据保留策略清理。

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
| 设备 Control WebSocket 断开 | 指数退避重连。保留终态 ACK |
| 设备 Realtime Audio 断开 | 结束当前 ASR session。回到 Monitoring |
| Morrow Dialogue Stream 断开 | 保存双方 ACK 水位。停止发送新帧。指数退避恢复 |
| Morrow 流恢复成功 | 从最后连续 ACK 后重放。按 frame、turn 和 segment 去重 |
| Morrow 流恢复失败 | 建立新 stream epoch。保留 conversation 和 turn 幂等关系 |
| Morrow 长时间不可用 | 当前交互播放本地离线提示。未提交 turn 过期，不延迟数小时回复 |
| Server 长时间不可用 | Morrow 持久保存未确认主动 turn，恢复后按原 event_id 重放 |
| Morrow 重复帧 | 数据库按 frame_id 去重，不重复执行工具或设备交付 |
| Morrow 非法结构化块 | 拒绝该块并通过同一流返回 turn error |
| 流背压 | 暂停普通 delta。优先 ACK、cancel、heartbeat 和 committed 状态 |
| DJI 拔出 | 当前 utterance `source_lost`。切回内置麦克风 |
| DJI 枚举失败 | 内置麦克风继续工作。后台限次重试 |
| TTS 超时 | 中止当前 speech。返回 failed。可播放本地错误音 |
| Camera 失败 | 进入 Maintenance 重建一次。再次失败禁用视觉 |
| I2C 错误 | 统一维护恢复。禁止模块自行重建 |
| Servo 无响应 | 停止动作。记录故障。限制后续 motion |
| 内部 SRAM 低于阈值 | 停止低优先级任务。上传诊断。必要时安全重启 |
| PSRAM 最大块持续下降 | 停止视觉和长录音。记录碎片诊断 |
| brownout 重启 | 增加计数。降低负载。连续发生时禁用 USB 外设 |
| Server 重启 | 从 SQLite 恢复对话帧、delivery、命令和 ACK |
| Morrow 重启 | 从持久 scheduler、对话和幂等状态恢复，再恢复单一流 |
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

### 阶段 2：设备控制和命令可靠性

1. 新建 SQLite Command Store。
2. 建立设备 Control WebSocket。
3. 建立命令 lease、续租、ACK 和重投。
4. 加入 boot_id 和两级去重。
5. 旧 `/command` 仅保留人工调试用途。
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

### 阶段 5：Morrow 单一流式对话闭环

1. 实现 `morrow_stream_gateway`。
2. 建立 `dialogue_frames`、conversation、turn 和 committed segment 持久化。
3. 实现双向 sequence、ACK、heartbeat、背压和恢复。
4. 实现 `user.text.delta` 和 `user.text.commit`。
5. 实现 `assistant.text.delta`、`segment_commit` 和 `turn_commit`。
6. 实现结构化 presentation 和 delivery policy block。
7. 实现主动 assistant turn。
8. 实现 device result 和 delivery status 回传。
9. 删除 Morrow 到 Server 的 Delivery API、回调和状态查询接口。
10. 保证未提交 delta 不产生任何外部效果。

### 阶段 6：Morrow 业务和真实交付

1. 将日程、会议、出行、天气和健康能力迁移到 Morrow。
2. 将 scheduler 迁移到 Morrow，并持久化 event_id。
3. 将外部工具写操作接入持久幂等。
4. 以设备 `rendered` 作为机器人送达终点。
5. 设备离线和失败状态通过同一流返回 Morrow。
6. 由 Morrow 决定飞书备用通知。
7. 验证 Morrow 和 Server 重启后的 turn 恢复与去重。

### 阶段 7：本地唤醒和健康感知

1. 接入 WakeNet 或关键词模型。
2. Quiet 模式关闭普通音频上传。
3. 完成自定义唤醒词验收。
4. 接入 Server 侧 posture model。
5. 完成 chair ROI 标定。
6. 区分 sedentary 和 continuous presence 事件。

### 阶段 8：安全和量产准备

1. 删除硬编码凭据。
2. 接入 WSS、HTTPS 和设备认证。
3. 为 Morrow Dialogue Stream 增加服务身份认证。
4. 增加签名 OTA 和 rollback。
5. 增加 coredump 和诊断导出。
6. 完成 24 小时、7 天和故障注入测试。

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
8. Server 断开时 Quiet 仍可本地唤醒。

### 22.3 设备命令可靠性

1. 命令收到后设备立即重启。Server lease 超时后重投。
2. 非幂等命令重投时设备不重复执行。
3. Server 重启后未完成命令仍存在。
4. Control WebSocket 重连后 ACK 可以重放。
5. 长 TTS 正常续租，不产生重复播放。
6. 过期提醒不会在很久以后补播。
7. 多设备环境不会投递到错误设备。

### 22.4 Morrow Dialogue Stream

1. Morrow 与 Server 之间只有一条活动 WSS，不存在 Agent REST、RPC、Webhook 或第二条消息通道。
2. ASR partial 以 `user.text.delta` 发送，最终文本以 `user.text.commit` 发送。
3. `assistant.text.delta` 不触发 TTS。
4. `assistant.text.segment_commit` 持久化后才触发 TTS。
5. stream 在任意帧后断开，恢复后不会缺帧，也不会重复播放已交付 segment。
6. 重放同一 `frame_id`、`turn_id`、`segment_id` 不产生重复副作用。
7. Morrow 可以在没有 Server 请求的情况下，通过同一流发起 proactive assistant turn。
8. Server 可以通过同一流返回 `device_offline`、`rendered`、`failed` 和 `expired`。
9. 用户本地 stop 先停止硬件，再通过同一流取消 Morrow turn。
10. 背压时 ACK、cancel 和 heartbeat 不被普通 delta 阻塞。
11. 同一连接复用多个 conversation 时，turn 顺序和设备绑定正确。
12. Morrow 或 Server 重启后，主动提醒和工具写操作不重复。
13. 非法 presentation block 被拒绝，不执行机械动作。
14. 未提交 delta 在 turn cancel 后被清理。

### 22.5 Camera 和 motion

1. camera 连续拍摄 1000 张，无持续 heap 下降。
2. vision 任务中 stop 可以在一轮边界生效。
3. 找不到人脸时返回 `not_found`，不播放成功话术。
4. 时间敏感提醒在 find_owner 失败后按 policy 继续。
5. 舵机超范围命令在入队前拒绝。
6. 软件角度、raw 目标和反馈角度一致。
7. servo 断线后进入明确 Fault。

### 22.6 内存和性能

1. 最坏组合场景最低内部空闲内存不少于 64 KiB。
2. 最大内部连续块不少于 48 KiB。
3. 最大 DMA 连续块不少于 32 KiB。
4. 最大 PSRAM 连续块不少于 512 KiB。
5. 所有任务剩余栈不少于 1 KiB 和 20%。
6. 音频热路径动态分配为零。
7. 图像上传期间设备 Control WebSocket stop 延迟符合要求。
8. ASR 音频上传期间 UI 和动作不影响音频连续性。

### 22.7 电源

1. DJI 加最大音量不会 brownout。
2. DJI 加双舵机启动不会 brownout。
3. DJI、TTS、camera upload 和舵机组合运行不会重启。
4. 低电量场景能够正确限制负载。
5. USB 热插拔不会造成设备复位。
6. 连续 brownout 策略能够回退内置麦克风。

### 22.8 Morrow 业务和交付

1. Morrow Scheduler 发起提醒后，只有设备 `rendered` 才标记机器人 delivered。
2. 设备离线状态通过同一流返回 Morrow。
3. Morrow 根据 delivery status 决定等待、取消或发送飞书备用通知。
4. 飞书日程创建在 Morrow 进程重启和流重放后不会重复。
5. 迟到通知在重复事件下只发送一次。
6. 当前会议上下文可以支持省略对象的二次指令。
7. 路线或天气失败时不编造结果。
8. 姿态模型不可用时不输出 `sedentary_detected`。
9. 健康提醒遵守会议、通话和 cooldown 策略。
10. Server 不直接调用 Morrow 内部工具。
11. Morrow 不通过调试 `/command` 接口控制设备。
12. 主动 turn 在 Morrow 断线重连后按原 event_id 恢复。

### 22.9 OTA 和安全

1. 错误签名固件被拒绝。
2. OTA 下载中断后仍能启动旧版本。
3. 新版本自检失败后自动 rollback。
4. token 和 Wi-Fi 密码不出现在源码和日志。
5. Quiet 模式普通语音不上传。
6. 图片默认不被持久保存。
7. 设备认证失败时不能接收命令。
8. Morrow 身份认证失败时不能建立 Dialogue Stream。
9. Morrow 和 Server 之间不存在共享数据库凭据。

### 22.10 长时间运行

1. 24 小时连续运行，无崩溃和明显资源下降。
2. 7 天连续运行，无持续 heap 或 PSRAM 最大块下降。
3. 7 天内反复执行语音、拍照、动作、USB 插拔、网络重连和 Morrow 流恢复。
4. watchdog、stack overflow、brownout 和 coredump 统计为零或符合故障注入预期。
5. 7 天内不存在重复主动提醒、重复日程创建或重复飞书通知。

---

## 23. 发布门槛

### 23.1 Demo 版本

必须完成：

- Supervisor。
- 基础设备命令队列。
- 内置和 DJI 稳定切换。
- 语音对话。
- Morrow Dialogue Stream 基本双向增量传输。
- 飞书日程和通知。
- 基础 camera 和 motion。
- 8 小时运行测试。

### 23.2 办公试点版本

必须完成：

- 持久 Command Store。
- 设备 Control WebSocket。
- Morrow 单一流的 sequence、ACK、恢复和背压。
- committed segment 到设备 TTS 的低延迟链路。
- 主动 assistant turn。
- lease、ACK、重试和去重。
- 主动提醒真实 `rendered` 闭环。
- Morrow 外部工具持久幂等。
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
- Morrow 与 Server 单一流通信约束自动化检查。
- Morrow、Server 和设备三端持久幂等。
- 多设备绑定。
- 姿态模型和健康提醒语义校验。
- 量产电源和舵机标定流程。
- 自动化硬件回归测试。
- 主动提醒、日程和飞书写操作在断线重放中无重复。

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
      morrow_stream_gateway.py
      dialogue_store.py
      turn_assembler.py
      render_coordinator.py
      command_store.py
      delivery_coordinator.py
      asr_service.py
      tts_service.py
      vision_service.py
      posture_service.py
      ota_service.py
    migrations/
    tests/

  morrow-agent/
    app/
      dialogue_stream/
        client.py
        protocol.py
        resume_store.py
      skills/
        agenda_briefing/
        calendar_assistant/
        meeting_reminder/
        travel_planner/
        wellbeing_companion/
      tools/
        lark_calendar/
        lark_contacts/
        lark_im/
        weather/
        route/
      scheduler/
      memory/
      idempotency/
    tests/

  shared/
    dialogue_protocol/
      frames.schema.json
      presentation.schema.json
      delivery_policy.schema.json
      error_codes.md

  docs/
    architecture_v3_1_morrow.md
    morrow_dialogue_stream_v1.md
    device_protocol_v3.md
    hardware_test_plan.md
    deployment.md
```

协议定义由 `shared/dialogue_protocol` 统一维护。

Morrow 和 Xiaopai Server 的流帧类型、sequence、ACK、恢复、turn、segment、presentation、delivery policy 和错误码必须由同一份 schema 生成或校验。

固件和 Xiaopai Server 的设备命令枚举、状态枚举和版本号必须由设备协议 schema 生成或校验。

Morrow 与 Xiaopai Server 的代码审查和自动化测试必须验证：Agent 边界只存在 `MorrowDialogueStreamAdapter`，不存在第二种通信实现。
