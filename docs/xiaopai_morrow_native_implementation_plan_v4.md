# Xiaopai / Stack-chan 完全适配 Morrow 的实施方案

版本：V4.0  
日期：2026-07-10  
适用代码：`repo(5).zip`  
适用 Morrow：机器人特别版 `v0.1.2-robot.2`

---

## 0. 文档定位

本方案以两类事实为唯一设计依据：

1. Morrow 当前已经提供并公开说明的 HTTP 与 WebSocket 服务。
2. `repo(5).zip` 中 Xiaopai Server 与 CoreS3 固件当前已经实现的能力。

本方案不要求修改 Morrow，不为 Morrow 假设尚不存在的协议，也不再保留 OpenClaw、OpenAI Chat Completions、自定义 Agent Stream 或其他 Agent 兼容层。

本方案中“完全只使用 Morrow”的含义是：

- Morrow 是系统中唯一的业务 Agent。
- 对话理解、上下文、飞书、天气、路线、主动提醒和业务工具只由 Morrow 完成。
- Xiaopai Server 不再直接调用飞书、天气、高德或另一套 Agent。
- Xiaopai Server 只负责音频、设备控制、文本转语音和可靠交付。
- CoreS3 固件只负责采集、播放、显示、动作和硬件状态。
- ASR、TTS、视觉和 OTA 仍由 Xiaopai Server 提供，因为 Morrow 当前对外接口是文本接口，不提供设备音频流、TTS 音频流或硬件驱动。

---

## 1. 最终结论

旧方案中的以下设计必须删除：

- 自定义 `Morrow Dialogue Stream`。
- `/morrow/dialogue/stream`。
- 自定义 `protocol_version`、`seq`、`ack`、`connection_epoch` 和 `resume_token`。
- 自定义 `assistant.text.segment_commit`。
- 自定义结构化表情、动作和 delivery policy block。
- Server 向 Morrow 上报 `device_result`、`delivery.status` 或命令 ACK。
- Server 与 Morrow 之间自定义的帧持久化和恢复协议。
- OpenClaw 命名和配置。
- 兼容 OpenAI Chat Completions 的代码。
- 让 Morrow 通过 `/v3/deliveries` 下发结构化设备任务的路径。

Morrow 与 Xiaopai Server 之间只保留 Morrow 当前支持的一个 session WebSocket：

```text
ws://127.0.0.1:3000/api/sessions/default/ws
```

Server 只向 Morrow 发送：

```text
start_turn
cancel_turn
reset_session
```

其中生产对话只需要 `start_turn`。`cancel_turn` 只有在拿到 Morrow 返回的有效 `turn_id` 时才发送。`reset_session` 只作为管理操作使用。

Server 只消费 Morrow 返回的：

```text
snapshot
agent_event
robot_notice
turn_saved
turn_rejected
error
```

---

## 2. Morrow 服务的硬约束

### 2.1 启动方式

必须使用机器人模式：

```bash
morrow server --robot --host 0.0.0.0 --port 3000
```

不能使用普通 CLI 模式代替。只有机器人模式才会加载飞书、天气、路线和主动提醒能力。

启动前检查：

```bash
morrow robot doctor
```

建议版本：

```text
v0.1.2-robot.2
```

### 2.2 固定 session

第一阶段只使用：

```text
default
```

WebSocket：

```text
ws://127.0.0.1:3000/api/sessions/default/ws
```

原因：

1. Morrow 的主动提醒只广播到启动时的默认 session。
2. 当前 Xiaopai 是单用户、单机器人产品形态。
3. 同一个 session 共享一份 Morrow 上下文。
4. 当前 Morrow 不提供 Xiaopai 所需的跨 session 主动提醒路由协议。

因此不得继续默认使用：

```text
xiaopai
xiaopai-{device_id}
```

也不得由设备 ID 自动创建 Morrow session。

若后续需要多用户隔离，必须先让 Morrow 支持主动提醒到指定 session，或为每个用户独立运行 Morrow 实例。不能由 Xiaopai Server 自行虚构多路复用协议。

### 2.3 Morrow 接收的消息

发起一轮对话：

```json
{
  "type": "start_turn",
  "data": {
    "request_id": "uuid",
    "prompt": "用户最终文本"
  }
}
```

禁止添加未公开字段：

```text
conversation_id
device_id
turn_id
model
backend_model
max_tokens
delivery_policy
```

取消一轮对话：

```json
{
  "type": "cancel_turn",
  "data": {
    "turn_id": "从 Morrow snapshot 获得的有效 turn_id"
  }
}
```

不得用 `request_id`、`conversation_id` 或本地生成的 ID 冒充 Morrow `turn_id`。

重置会话：

```json
{
  "type": "reset_session",
  "data": {
    "request_id": "uuid"
  }
}
```

重置属于管理操作。设备普通触摸、停止播放或新一轮对话不得自动重置 Morrow 上下文。

### 2.4 Morrow 返回的消息

连接建立后先收到 `snapshot`。

流式文本：

```json
{
  "type": "agent_event",
  "data": {
    "event": {
      "type": "text_delta",
      "data": "正在为你查询"
    }
  }
}
```

最终文本：

```json
{
  "type": "agent_event",
  "data": {
    "event": {
      "type": "agent_message",
      "data": "完整回复"
    }
  }
}
```

主动提醒：

```json
{
  "type": "robot_notice",
  "data": {
    "id": "meeting:event-id:5",
    "timestamp_ms": 1783250343202,
    "kind": "meeting_reminder",
    "text": "你有一个会议将在5分钟后开始，主题是项目会。请提前准备。"
  }
}
```

一轮保存完成：

```json
{
  "type": "turn_saved",
  "data": {
    "session": "default",
    "turn_index": 12
  }
}
```

拒绝：

```json
{
  "type": "turn_rejected",
  "data": {
    "request_id": "uuid",
    "reason": "session has a running turn"
  }
}
```

错误：

```json
{
  "type": "error",
  "data": {
    "message": "错误说明"
  }
}
```

### 2.5 单 session 单进行中 turn

Morrow 在同一个 session 有正在运行的 turn 时，会拒绝新的 `start_turn`。

因此 Xiaopai Server 必须实现一个全局 `MorrowTurnCoordinator`：

```text
所有设备 ASR 最终文本
        |
        v
有界 FIFO 请求队列
        |
        v
同一时刻只允许一个 start_turn
        |
        v
等待 turn_saved、turn_rejected、error 或超时
```

不得再使用多个 worker 并发调用同一个 Morrow session。

---

## 3. 目标架构

```text
                           Morrow Robot Agent
             对话、上下文、飞书、天气、路线、主动提醒
                                  |
                   Morrow Session WebSocket
             /api/sessions/default/ws，单连接，单 turn
                                  |
                                  v
                         Xiaopai Server
        +-------------------------+-------------------------+
        |                         |                         |
 MorrowClient             MorrowTurnCoordinator     NoticeDispatcher
 连接、收发、重连          排队、流式切句、取消代际       去重、TTL、入队
        |                         |                         |
        +-------------------------+-------------------------+
                                  |
                           CommandStore
                    命令持久化、租约、ACK、重试
                                  |
             +--------------------+--------------------+
             |                    |                    |
   Realtime Audio WS       Device Long Poll       Bulk HTTP
   音频上传和 ASR          命令和 ACK             TTS、视觉、OTA
             |                    |                    |
             +--------------------+--------------------+
                                  |
                              CoreS3
        本地唤醒、录音、播放、表情、舵机、摄像头、触摸、OTA
```

### 3.1 唯一业务 Agent

Server 不得包含以下业务实现：

- 飞书日程查询。
- 飞书日程创建。
- 联系人搜索。
- 飞书消息发送。
- 天气查询。
- 路线规划。
- 会议规则判断。
- 外勤规则判断。
- 出差规则判断。
- 主动提醒扫描器。
- 工作复盘逻辑。
- 另一套模型调用。

这些能力全部通过用户自然语言交给 Morrow。

### 3.2 Server 的职责

Server 只负责：

- 维护一条 Morrow WebSocket。
- 串行提交用户最终文本。
- 消费 Morrow 文本增量。
- 按标点及时切分 TTS。
- 消费并可靠投递 `robot_notice`。
- 管理设备在线状态。
- 管理设备命令、重试和 ACK。
- ASR。
- TTS。
- 低频视觉。
- OTA。
- 本地系统事件和诊断。

### 3.3 固件的职责

固件只负责：

- 本地唤醒和触摸。
- 录音、VAD、音频源管理。
- 上传音频。
- 接收设备命令。
- 播放 TTS。
- 表情、灯带和舵机。
- 拍照与上传。
- 命令去重。
- ACK 和终态重放。
- 故障恢复。
- OTA。

---

## 4. 当前代码审计结论

### 4.1 已经可以保留的实现

当前代码已经具备以下基础：

- Morrow WebSocket 客户端雏形。
- `start_turn` 调用。
- `text_delta` 消费。
- `agent_message` 消费。
- 按标点切分。
- `robot_notice` 监听。
- SQLite WAL。
- 设备命令表、attempt、lease 和 ACK 表。
- 设备长轮询命令通道。
- 实时音频 WebSocket。
- ASR 最终文本。
- TTS 流式播放。
- Control hello。
- 固件命令状态和基本 ACK。
- 摄像头、舵机、表情、触摸和 OTA 基础。
- 当前 Server 测试基线为 70 项通过。

这些实现应重构，不需要整体推倒重写。

### 4.2 当前必须修正的问题

| 问题 | 影响 | 处理 |
|---|---|---|
| 文件和变量仍叫 OpenClaw | 边界不清，配置混乱 | 全量重命名为 Morrow |
| 默认 session 是 `xiaopai` | 收不到默认 session 的主动提醒 | 固定为 `default` |
| `start_turn` 携带 `conversation_id` | 发送 Morrow 未公开字段 | 删除 |
| `cancel_turn` 携带错误字段 | 无法按 Morrow 协议取消 | 只发送有效 `turn_id` |
| 发送 `device_result` | Morrow 不支持 | 删除 |
| 多 worker 并发调用 Morrow | 产生 `turn_rejected` | 改为单 coordinator 串行 |
| `robot_notice` 只保留 text | 丢失权威 ID、kind 和时间 | 保存完整 data |
| notice 生成本地随机 ID | 无法正确去重 | 使用 Morrow `data.id` |
| notice 入队即视为完成 | 不是真实播放完成 | 关联命令 ACK |
| HTTP long poll 和 MCP 双命令通道 | 可能重复、乱序、ACK 不一致 | 只保留 long poll |
| 固件排队前先回 received | 队列满时出现假接收 | 成功准入后再 ACK |
| 固件 ACK 固定 attempt=0 | 重试语义不完整 | 回传原 attempt |
| 固件无 cmd_id 去重 | 重连可能重复执行 | 增加 RAM/NVS 去重 |
| TTS 使用 GET query | 长文本和编码风险 | 改用 `POST /v3/tts` |
| Server 保存自定义 delivery | Morrow 无法调用 | 从生产链路删除 |
| health 仍显示 openclaw | 运行状态误导 | 改为 morrow |
| OpenClaw model 配置仍存在 | 对 Morrow 无效 | 删除 |

---

## 5. MorrowClient 设计

### 5.1 单一所有者

整个 Server 进程只创建一个 `MorrowClient`。

以下模块不得各自创建客户端：

- HTTP Handler。
- RealtimeServer。
- Notice worker。
- ASR worker。
- Delivery worker。

统一依赖：

```python
app_state.morrow_client
app_state.morrow_coordinator
```

### 5.2 连接状态机

```text
DISCONNECTED
-> CONNECTING
-> WAITING_SNAPSHOT
-> READY
-> TURN_RUNNING
-> READY
```

异常路径：

```text
任意连接状态
-> DISCONNECTED
-> 指数退避
-> CONNECTING
```

建议退避：

```text
1 s
2 s
4 s
8 s
16 s
30 s
30 s ...
```

连接成功后必须先收到 `snapshot`，再把 Morrow 标记为 ready。

### 5.3 启动检查

Server 启动时执行：

1. `GET /api/status`。
2. 检查 Morrow 可达。
3. 连接 `/api/sessions/default/ws`。
4. 等待 `snapshot`。
5. 启动 notice 消费。
6. 开放用户对话入口。

若 Morrow 未连接：

- ASR 仍可运行。
- 设备仍可报告状态。
- 用户最终文本不直接丢弃。
- 最多保留一个短时待提交请求。
- 超过请求 TTL 后明确播报“服务暂时不可用”，不得无限积压。
- 主动提醒无法补拉，因此连接告警必须进入 health。

### 5.4 收包线程不得阻塞

WebSocket reader 只做：

- JSON 解析。
- 类型校验。
- 投递到有界内部队列。
- 更新连接状态。

不得在 reader 中直接：

- 调 TTS。
- 写设备长轮询。
- 等数据库事务。
- 执行视觉。
- 等待队列空位。

`robot_notice` 队列满时，不得阻塞 WebSocket reader。应先同步写入轻量数据库事务，或使用专用有界 spool。若仍失败，记录高优先级告警。

### 5.5 不实现虚假的恢复协议

当前 Morrow 外部服务说明没有提供：

- 消息 sequence。
- 消息 ACK。
- turn replay。
- resume token。
- 增量重放。
- 已发送 prompt 的幂等执行保证。

因此：

- 断线后重新建立 WebSocket。
- 等待新 `snapshot`。
- 不自动重发断线前的 `start_turn`。
- 不自动重发可能触发写操作的用户 prompt。
- 已经转换为设备命令的文本片段继续按本地规则交付。
- 断线后未收到的剩余回复视为中止。
- 用户可以重新发起新一轮请求。
- 对日程创建、发送消息等不可逆操作，绝不能自动重放 prompt。

---

## 6. MorrowTurnCoordinator

### 6.1 输入结构

```python
@dataclass
class MorrowRequest:
    request_id: str
    prompt: str
    device_id: str
    source: str
    created_at: float
    expires_at: float
    generation: int
```

`source` 只取：

```text
voice
touch
system
admin
```

普通设备事件不应自动送给 Morrow。只有确实需要业务理解的事件，才转换成清晰自然语言 prompt。

### 6.2 队列

建议：

```text
容量：8
并发 turn：1
普通语音 TTL：60 s
系统请求 TTL：按业务配置
```

队列满时：

- 不覆盖正在运行的请求。
- 不静默丢弃。
- 对新的普通语音返回本地忙提示。
- 同一设备的重复、未开始请求可以合并为最新一条，但不能合并已经提交给 Morrow 的请求。

### 6.3 turn 生命周期

```text
QUEUED
-> SUBMITTED
-> STREAMING
-> SAVED
```

异常终态：

```text
REJECTED
ERROR
TIMEOUT
CANCELLED
DISCONNECTED
EXPIRED
```

只有收到 `turn_saved` 后，才允许发送下一条 `start_turn`。

若收到 `turn_rejected`：

1. 根据 `request_id` 关联当前请求。
2. 记录原因。
3. 不立即并发重发。
4. 等当前 Morrow turn 结束或重新连接后再决定是否重试。
5. 可能触发写操作的 prompt 不自动重试。

### 6.4 事件关联

Morrow 的 `text_delta`、`agent_message` 和 `turn_saved` 不保证都带本地 `request_id`。

因此当前 session 必须只存在一个活动请求。所有无 request_id 的 turn 事件都归属当前唯一活动请求。

这不是临时 workaround，而是适配当前 Morrow session 语义的必要约束。

---

## 7. 流式文本与 TTS

### 7.1 核心规则

Morrow 的流式文本一旦发出，可以立即进入本地切句缓冲。

不需要等待：

- `agent_message`。
- `turn_saved`。
- 自定义 commit。
- 自定义帧 ACK。

流程：

```text
text_delta
-> 追加字符
-> 遇到完整标点或长度阈值
-> 生成 speak 命令
-> 写入 CommandStore
-> 设备获取并播放
```

### 7.2 切句规则

第一阶段标点：

```text
。！？!?；;
```

逗号、顿号和冒号不默认立即切句，避免过短语音和语调破碎。

可在以下条件使用软切句：

- 缓冲超过 80 个中文字符。
- 连续 1.0 秒没有新 delta。
- 当前片段已经超过 40 个字符并遇到逗号。

硬上限同时检查字符数和 UTF-8 字节数：

```text
建议字符上限：120
建议 UTF-8 上限：480 bytes
固件 text buffer 至少：768 bytes
```

不得只按 Python 字符数切分后直接写入固定 C 缓冲。

### 7.3 `agent_message` 去重

规则：

- 若本轮已经收到至少一个 `text_delta`，`agent_message` 只用于日志和一致性检查，不再重复播报。
- 若本轮完全没有 `text_delta`，则对 `agent_message` 执行同样的切句流程。
- 收到 `turn_saved` 后 flush 剩余非空文本。
- 若收到 error 或断线，也可以 flush 已经收到且语义完整的尾部，但不得补造未收到内容。

### 7.4 顺序

每个 speak 命令保存：

```text
source_type = dialogue
source_id = request_id
segment_index = 0, 1, 2...
turn_generation
```

同一请求按 `segment_index` 严格播放。

不同请求不交叉播放。

### 7.5 停止

用户长按 stop 时：

1. 固件立即停止当前播放。
2. 固件清空本地旧 generation 的待播放语音。
3. Server 取消该请求尚未 lease 的 speak 命令。
4. Server 增加 conversation generation。
5. 后续到达的旧 turn delta 只读取并丢弃，不再生成命令。
6. 若 `snapshot.running_turn` 中获得有效 `turn_id`，向 Morrow 发送 `cancel_turn`。
7. 若没有有效 `turn_id`，不构造伪 ID。等待该 turn 自然结束，同时忽略其输出。

---

## 8. 主动提醒

### 8.1 数据源

主动提醒只来自 Morrow：

```text
robot_notice
```

Server 不再自行扫描飞书日历，也不复刻 Morrow 的会议、外勤或出差判断。

### 8.2 完整保存

收到 notice 后必须保存：

```text
notice_id
timestamp_ms
kind
text
received_at
expires_at
state
command_id
rendered_at
last_error
```

`notice_id` 必须直接使用：

```text
data.id
```

不得重新生成随机 ID。

### 8.3 去重

`notice_id` 是数据库主键。

重复收到相同 notice：

- 若状态为 `pending`、`queued`、`leased` 或 `rendered`，不再生成第二条命令。
- 若状态为 `failed`，只按本地重试策略处理，不把重复 WebSocket 消息当成新提醒。
- 重启后仍应去重。

### 8.4 kind 映射

第一阶段只使用 Morrow 当前说明的 kind：

| kind | 本地优先级 | 默认 TTL | 是否打断 |
|---|---:|---:|---|
| `meeting_reminder` | 80 | 10 分钟 | 否 |
| `fieldwork_reminder` | 70 | 30 分钟 | 否 |
| `travel_reminder` | 60 | 6 小时 | 否 |
| 未知 kind | 50 | 30 分钟 | 否 |

普通提醒不得打断正在进行的用户录音或正在播放的普通回复。

当提醒到达时：

- 若正在录音，延后到当前对话结束。
- 若正在说话，排在当前完整语句之后。
- 若设备离线，在 TTL 内等待设备上线。
- 若过期，标记 `expired`，不补播陈旧提醒。

### 8.5 真实完成状态

notice 状态：

```text
received
-> queued
-> leased
-> rendered
```

异常：

```text
failed
expired
cancelled
```

只有设备返回 speak 命令的 `rendered` ACK，notice 才能标记为 `rendered`。

Morrow 当前没有设备交付回传接口，因此 Server 不向 Morrow发送：

```text
device_result
delivery.status
notice_outbox
rendered
failed
```

这些状态只保存在 Xiaopai Server，用于诊断和产品指标。

### 8.6 主动提醒不保证断线补发

当前 Morrow 文档只说明实时广播，没有说明历史 notice 拉取或断线重放。

因此：

- Server 必须长期保持 `default` session WebSocket 在线。
- 断线期间可能漏掉主动提醒。
- Server 必须监控连接并快速重连。
- 不能在方案中承诺 Morrow 断线期间的 notice exactly-once。
- 本地持久化只能保证“已经收到的 notice”可靠交付，不能恢复“从未收到的 notice”。

---

## 9. Server 详细修改

### 9.1 文件重构

建议目标目录：

```text
src/
  app_state.py
  morrow_client.py
  morrow_coordinator.py
  morrow_protocol.py
  notice_dispatcher.py
  database.py
  command_store.py
  command_dispatcher.py
  control_gateway.py
  realtime_server.py
  asr_service.py
  tts_service.py
  vision_service.py
  ota_service.py
  schemas.py
  server.py
```

### 9.2 `openclaw_agent.py`

重命名：

```text
src/openclaw_agent.py
->
src/morrow_client.py
```

删除：

- `SYSTEM_PROMPT`。
- `xiaopai_openclaw_prompt` 依赖。
- `extract_openclaw_text`。
- OpenAI Chat Completions 兼容代码。
- `conversation_id`。
- `_active_by_conversation`。
- `build_openclaw_session_key`。
- `build_morrow_conversation_id`。
- `send_device_result`。
- 自定义 `device_result` 消息。
- 错误的 cancel payload。
- class-level 按配置共享多 client 的复杂逻辑。

保留并改写：

- `build_morrow_ws_url`。
- WebSocket reader/writer。
- `snapshot`。
- `agent_event`。
- `robot_notice`。
- `turn_saved`。
- `turn_rejected`。
- `error`。
- 连接状态。
- 指数退避重连。
- 有界消息队列。

### 9.3 新增 `morrow_protocol.py`

集中定义严格 schema：

```python
def build_start_turn(request_id: str, prompt: str) -> dict:
    return {
        "type": "start_turn",
        "data": {
            "request_id": request_id,
            "prompt": prompt,
        },
    }
```

```python
def build_cancel_turn(turn_id: str) -> dict:
    return {
        "type": "cancel_turn",
        "data": {
            "turn_id": turn_id,
        },
    }
```

解析器对未知事件：

- 记录 debug 日志。
- 不崩溃。
- 不把未知事件当成文本。
- 工具事件仅用于日志和可观测性。

### 9.4 新增 `morrow_coordinator.py`

职责：

- 管理唯一活动 turn。
- 管理请求 FIFO。
- 生成 UUID request_id。
- 提交 `start_turn`。
- 消费 delta。
- 进行切句。
- 创建 speak 命令。
- 等待 `turn_saved`。
- 处理 cancel generation。
- 处理超时、拒绝和断线。

不得在 `server.py` 或 `realtime_server.py` 中直接调用 `MorrowClient.chat_stream()`。

### 9.5 修改 `realtime_server.py`

保留：

- 设备实时音频 WebSocket。
- hello。
- 音频帧。
- ASR partial。
- ASR final。
- 设备状态消息。

修改：

- ASR final 只调用 `MorrowTurnCoordinator.submit()`。
- 不再自行创建 `OpenClawAgent`。
- 不再直接持有 Morrow WebSocket。
- 不再自行切分 Morrow 文本。
- 不再拥有第二套 speak 投递逻辑。

删除生产路径：

- `McpRequestTracker`。
- `command_to_mcp_calls`。
- `_send_mcp_command`。
- MCP tool call 回退。
- 通过 realtime WS 下发动作和 speak。

Realtime Audio WS 的职责必须收敛为：

```text
audio upload
ASR session state
ASR partial/final
device interaction state
```

### 9.6 修改 `server.py`

当前文件过大，第一阶段可以保留入口，但需要完成以下收敛：

- 只实例化一个 `MorrowClient`。
- 只实例化一个 `MorrowTurnCoordinator`。
- 删除 `ThreadPoolExecutor` 并发 Morrow 调用。
- 删除 `STACKCHAN_OPENCLAW_WORKERS`。
- 删除 `send_device_result`。
- 删除自定义 notice outbox 回传 Morrow。
- notice listener 传递完整 `data`，不只传 text。
- health 字段改为 `morrow`。
- readiness 必须反映 snapshot 是否已经收到。
- 所有 speak 命令只进入 `CommandStore`。
- 不再调用 MCP 回退。
- 管理接口和调试接口不得被描述为 Morrow 边界。

### 9.7 修改 `command_store.py`

命令增加：

```text
source_type
source_id
segment_index
turn_generation
payload_retention_until
```

唯一约束建议：

```text
UNIQUE(source_type, source_id, segment_index, command_type)
```

增加方法：

```text
cancel_pending_by_source(source_type, source_id)
cancel_pending_before_generation(device_id, generation)
find_terminal_ack(cmd_id)
```

同一 turn 的 speak 顺序按：

```text
created_at
segment_index
```

不能只依赖 priority。

### 9.8 修改 `database.py`

保留：

```text
devices
device_sessions
commands
command_attempts
command_acks
captures
ota_releases
```

将 `morrow_notices` 迁移为：

```text
notice_id TEXT PRIMARY KEY
kind TEXT NOT NULL
timestamp_ms INTEGER NOT NULL
text TEXT NOT NULL
state TEXT NOT NULL
expires_at TEXT
command_id TEXT
received_at TEXT NOT NULL
rendered_at TEXT
last_error TEXT
```

删除或停用生产路径中的：

```text
deliveries
自定义 dialogue_frames
自定义 agent_stream_sessions
自定义 committed_segments
```

若历史表已经存在，可保留表结构一个迁移版本，但应用代码不得继续写入。

### 9.9 删除 `delivery_coordinator.py`

Morrow 当前不发送结构化：

```text
speech
presentation
actions
delivery policy
```

因此 `DeliveryCoordinator` 不应作为 Morrow 生产链路的一部分。

若要保留人工调试，可移动到：

```text
tools/manual_delivery.py
```

并明确标记为非 Morrow 接口。

### 9.10 删除 OpenClaw 配置

删除：

```text
OPENCLAW_BASE_URL
OPENCLAW_GATEWAY_TOKEN
STACKCHAN_OPENCLAW_BASE_URL
STACKCHAN_OPENCLAW_GATEWAY_TOKEN
STACKCHAN_OPENCLAW_MODEL
STACKCHAN_OPENCLAW_BACKEND_MODEL
STACKCHAN_OPENCLAW_WORKERS
STACKCHAN_OPENCLAW_MAX_COMPLETION_TOKENS
STACKCHAN_OPENCLAW_SESSION_PREFIX
```

新增：

```text
MORROW_BASE_URL=http://127.0.0.1:3000
MORROW_SESSION=default
MORROW_CONNECT_TIMEOUT_SECONDS=10
MORROW_TURN_TIMEOUT_SECONDS=120
MORROW_RECONNECT_MIN_SECONDS=1
MORROW_RECONNECT_MAX_SECONDS=30
MORROW_AUTH_TOKEN=
```

`MORROW_AUTH_TOKEN` 默认空。只有在反向代理额外加鉴权时才使用。

---

## 10. Server 与设备的唯一控制通道

### 10.1 保留 HTTP long poll

设备命令只通过：

```http
GET /v3/device/next-command
```

设备 ACK 只通过：

```http
POST /v3/command_ack
```

连接握手：

```http
POST /v3/control/hello
```

### 10.2 删除第二命令通道

禁止再通过 realtime audio WebSocket 下发：

- speak。
- face。
- motion。
- find_owner。
- OTA。
- MCP tool call。
- stop 之外的设备控制。

stop 也应进入同一命令存储和控制通道。只有本地物理长按 stop 可以绕过网络，在设备本地立即执行。

### 10.3 Bulk HTTP

继续保留：

```text
POST /v3/tts
POST /v3/vision/captures
GET  /v3/ota/manifest
GET  /v3/ota/images/{version}
```

图片、TTS 和 OTA 不经过设备控制长轮询正文传输。

---

## 11. 设备命令协议

### 11.1 命令

```json
{
  "cmd_id": "cmd_uuid",
  "type": "speak",
  "priority": 50,
  "ttl_ms": 30000,
  "attempt": 1,
  "coalesce_key": "",
  "safety_class": "normal",
  "turn_id": "request_uuid",
  "admission": {
    "allow_in_quiet": false,
    "defer_during_recording": true,
    "defer_during_speaking": true,
    "presence_requirement": "none"
  },
  "payload": {
    "text": "五分钟后有项目会议。",
    "segment_index": 0,
    "generation": 42
  }
}
```

`turn_id` 在设备协议中可以使用 Xiaopai `request_id`。它不是 Morrow 的内部 `turn_id`，两者必须在代码命名中区分：

```text
morrow_turn_id
xiaopai_request_id
```

### 11.2 ACK

```json
{
  "type": "command_ack",
  "ack_seq": 1025,
  "device_id": "xiaopai_xxx",
  "boot_id": 12345,
  "cmd_id": "cmd_uuid",
  "attempt": 1,
  "state": "rendered",
  "effect": "speech_played",
  "started_at_tick": 112233,
  "finished_at_tick": 115900,
  "message": ""
}
```

状态：

```text
received
running
rendered
done
failed
cancelled
expired
```

### 11.3 ACK 时机

`speak`：

```text
命令通过校验并成功进入 SpeechQueue
-> received

speech_task 真正开始取该命令
-> running

TTS 流结束、PCM 全部写入、播放队列 drain
-> rendered
```

队列满时不得先发 `received`，而应：

- 在有限时间内等待队列空位。
- 或不获取新命令。
- 或返回明确 `failed: queue_full`。

推荐做法是固件只有在本地有可用准入槽位时才请求下一条命令。

### 11.4 attempt

固件必须保存并回传 Server 下发的 `attempt`。

不得固定为：

```text
0
```

Server 用 `(cmd_id, attempt)` 判断租约与重试实例。

### 11.5 去重

固件维护：

1. RAM LRU，保存最近 64 个 `cmd_id` 和终态。
2. NVS 循环日志，保存最近 32 个非幂等命令。

重复命令：

- 若已经终态，直接重发原终态 ACK。
- 若正在执行，重发 `running`。
- 若已经进入队列，重发 `received`。
- 不重复播放、不重复动作、不重复 OTA。

### 11.6 ACK 重放

ACK POST 失败时：

- 终态 ACK 进入固定大小重放缓冲。
- 重连后先发送旧终态 ACK。
- 缓冲至少保存 32 条终态。
- RAM 缓冲不足时，关键非幂等命令终态写 NVS。
- ACK 重放不能再次执行命令。

---

## 12. CoreS3 固件修改

### 12.1 `main_command_services.inc`

修改命令解析结构，完整保存：

```text
cmd_id
attempt
turn_id
ttl_ms
received_monotonic_tick
deadline_tick
segment_index
generation
```

流程改为：

```text
收到 JSON
-> schema 校验
-> TTL 校验
-> cmd_id 去重
-> admission 校验
-> 成功进入目标队列
-> received ACK
```

不得在队列插入之前 ACK。

### 12.2 `main_tts_commands.inc`

`speak` item 增加：

```cpp
struct SpeechItem {
    char cmd_id[40];
    uint32_t attempt;
    uint32_t generation;
    uint16_t segment_index;
    uint64_t deadline_tick;
    char text[768];
};
```

执行前再次检查：

- deadline。
- generation。
- stop token。
- 当前 boot_id。
- 是否已被去重终态覆盖。

播放开始发 `running`。播放完成发 `rendered`。

### 12.3 TTS 改为 POST

由：

```http
GET /stream-speak?text=...
```

迁移到：

```http
POST /v3/tts
Content-Type: application/json
```

请求：

```json
{
  "device_id": "xiaopai_xxx",
  "cmd_id": "cmd_uuid",
  "text": "回复文本",
  "voice": "default",
  "sample_rate": 16000
}
```

设备边接收边播放，不缓存完整 WAV。

迁移完成后删除 legacy GET 路径。

### 12.4 固定缓冲

当前每次语音使用动态嵌套 vector 和 `new` 的部分应改为：

- 固定 PCM 环形缓冲。
- PSRAM 块池。
- 栈上或静态 pause guard。
- 固定大小 HTTP 读缓冲。
- 不在每个音频 chunk 中分配内存。

### 12.5 `main_realtime_transport.inc`

保留：

- 音频上传。
- ASR 会话控制。
- 服务端 ASR partial/final。
- ping/pong。
- 设备状态。

删除：

- MCP 工具注册。
- MCP tools/call。
- Server 通过 realtime WS 下发设备动作。
- MCP 结果跟踪。
- speak 的 realtime fallback。

### 12.6 stop generation

本地长按 stop：

```text
立刻停止 I2S 输出
-> 清空 SpeechQueue 中旧 generation
-> generation++
-> 发送 local_stop 事件给 Server
```

Server 收到后：

- 取消旧 generation 的未执行命令。
- 忽略旧 Morrow turn 后续 delta。
- 有合法 `morrow_turn_id` 时调用 `cancel_turn`。

### 12.7 心跳

长轮询请求可以更新在线时间，但仍建议每 5 秒发送轻量 heartbeat，携带：

```text
device_id
boot_id
mode
interaction_state
free_internal_heap
free_psram
speech_queue_depth
last_ack_seq
firmware_version
```

若保留 Control WebSocket，则 Control WebSocket 必须成为唯一控制通道，不能与 long poll 并行。第一阶段建议继续使用已经可工作的 HTTP long poll，避免同时重构两套协议。

### 12.8 保留的本地能力

以下能力不依赖 Morrow，继续保留：

- 本地唤醒词。
- Quiet。
- 触摸。
- DJI 与内置麦克风切换。
- VAD 和 pre-roll。
- 摄像头。
- find_owner。
- 舵机。
- 表情。
- 电源管理。
- OTA。
- 本地故障 stop。

但 Morrow 当前只返回文本，没有结构化动作意图。因此普通对话不得从自然语言中解析动作。

可以使用确定性的本地展示策略：

```text
开始录音 -> listening 表情
等待 Morrow -> thinking 表情
开始播放 -> speaking 表情
播放完成 -> neutral 表情
robot_notice -> notification 表情
错误 -> error 表情
```

这些属于 UI 状态映射，不是第二套 Agent。

---

## 13. 数据库设计

### 13.1 为什么仍需要数据库

Morrow 已经保存对话上下文，因此 Xiaopai Server 不需要保存完整聊天历史。

数据库仍然必要，用于设备交付可靠性：

- 设备命令先持久化再下发。
- 设备断线后可以在 TTL 内重试。
- 收到 ACK 后可以确认真实终态。
- 防止主动提醒重复播报。
- 防止 Server 重启后丢失已收到但尚未播放的提醒。
- 保存 OTA、设备 session 和诊断状态。

### 13.2 不存什么

不持久保存：

- 所有 `text_delta`。
- Morrow 完整 session history。
- 工具调用参数和结果的副本。
- 自定义 dialogue frame。
- 自定义 Morrow ACK。
- 自定义结构化 presentation。
- 用户音频长期原始数据，除非用户明确启用诊断采样。

### 13.3 存什么

```text
devices
device_sessions
commands
command_attempts
command_acks
morrow_notices
captures
ota_releases
schema_migrations
```

普通对话只保存已经形成的设备 speak 命令。可在命令 rendered 后按策略删除或脱敏 payload text。

建议默认：

```text
命令元数据：保留 30 天
普通对话文本：rendered 后 24 小时删除
主动提醒文本：保留 7 天
错误日志：保留 30 天
```

---

## 14. 删除清单

生产代码删除或退出生产路径：

```text
src/xiaopai_openclaw_prompt.py
src/delivery_coordinator.py
src/mcp_client.py
OpenClaw Chat Completions 兼容逻辑
Morrow device_result 发送逻辑
Morrow conversation_id 逻辑
自定义 Morrow stream frame
MCP 命令回退
/v3/deliveries 作为 Agent 接口
legacy /device/ack 回退
legacy GET /stream-speak
```

测试和文档中同步删除：

```text
OpenClaw 名称
openclaw/default 模型配置
自定义 Morrow Dialogue Stream
结构化 presentation block
delivery feedback to Morrow
自定义 seq/ack/resume
```

---

## 15. 保留清单

继续保留并修正：

```text
SQLite WAL
CommandStore
command attempts
command ACK
device hello
HTTP long poll
Realtime Audio WebSocket
ASR
POST /v3/tts
vision upload
OTA
local wake
DJI UAC
touch
servo
face UI
health metrics
```

---

## 16. 配置与部署

### 16.1 Morrow systemd

```ini
[Unit]
Description=Morrow Robot Agent
After=network.target

[Service]
WorkingDirectory=/home/gargantua/code/morrow
ExecStart=/home/gargantua/.local/bin/morrow server --robot --host 0.0.0.0 --port 3000
Restart=always
RestartSec=3
Environment=NO_PROXY=127.0.0.1,localhost

[Install]
WantedBy=multi-user.target
```

### 16.2 Xiaopai Server 环境变量

```bash
MORROW_BASE_URL=http://127.0.0.1:3000
MORROW_SESSION=default
MORROW_CONNECT_TIMEOUT_SECONDS=10
MORROW_TURN_TIMEOUT_SECONDS=120
MORROW_RECONNECT_MIN_SECONDS=1
MORROW_RECONNECT_MAX_SECONDS=30

STACKCHAN_DATABASE_PATH=/var/lib/xiaopai/xiaopai.db
STACKCHAN_COMMAND_LEASE_MS=15000
STACKCHAN_DEVICE_OFFLINE_TTL_MS=15000
STACKCHAN_TTS_SAMPLE_RATE=16000
```

### 16.3 启动顺序

```text
SQLite migration
-> CommandStore recovery
-> HTTP/Realtime services
-> GET Morrow /api/status
-> 连接 default session WS
-> 收到 snapshot
-> Morrow ready
-> 接受用户 turn
```

Morrow 暂时不可用时，Server 不应退出。它应保留设备服务并持续重连。

---

## 17. 实施阶段

### P0：冻结协议与建立基线

完成：

- 保存当前测试结果。
- 增加 Morrow 文档协议 fixture。
- 增加真实 WebSocket 消息样例。
- 明确 `default` session。
- 禁止新增 OpenClaw 兼容代码。

验收：

```text
现有 70 项测试保持通过
新增协议测试先覆盖 start_turn、delta、final、notice、saved、rejected、error
```

### P1：Morrow 客户端改正

完成：

- `openclaw_agent.py` 重命名。
- 删除错误字段。
- 删除 `device_result`。
- 固定 `default` session。
- 实现 snapshot readiness。
- 实现指数退避重连。
- 保留完整 robot_notice。

验收：

- 发出的 `start_turn` 只有 `request_id` 和 `prompt`。
- 不再出现 `conversation_id`。
- 不再出现 `device_result`。
- 连接 Morrow 后先收到 snapshot 才 ready。
- 重连不会自动重放 prompt。

### P2：单 turn coordinator

完成：

- 新增全局请求队列。
- 删除多 worker 并发。
- ASR final 统一提交 coordinator。
- `turn_saved` 前不发下一条 `start_turn`。
- 建立 generation cancellation。

验收：

- 连续快速提交 10 条 ASR，不出现 Morrow 并发 turn。
- 请求按顺序处理。
- 不出现 `session has a running turn`，异常测试除外。
- stop 后旧 delta 不再进入 TTS。

### P3：流式切句

完成：

- 统一 segmenter。
- delta 实时入队。
- final 去重。
- turn_saved flush。
- UTF-8 字节上限。
- 同 turn 严格顺序。

验收：

- 第一完整句无需等待最终消息即可播放。
- `agent_message` 不重复播报。
- 无 delta 时可以播报 final。
- 长回复不会溢出固件 text buffer。
- 网络断开时不补造文本。

### P4：主动提醒可靠投递

完成：

- 保存完整 notice。
- 使用 Morrow notice id 去重。
- kind 对应 TTL。
- notice 与 command_id 关联。
- rendered ACK 后更新状态。
- 删除 notice device_result 回传。

验收：

- 同一 notice 重复到达只播放一次。
- Server 重启后，已收到且未过期 notice 可以继续投递。
- 设备离线后在 TTL 内上线可以播放。
- 过期提醒不补播。
- 状态只有收到 rendered 后才完成。

### P5：设备单控制通道

完成：

- 删除 MCP 命令回退。
- realtime WS 只保留音频和 ASR。
- 所有设备命令进入 CommandStore。
- 长轮询和 ACK 成为唯一生产控制路径。

验收：

- 同一命令只可能从一个通道到达设备。
- 命令顺序可追踪。
- ACK 只有一个权威来源。
- 断开 realtime audio 不影响已入库设备命令。

### P6：固件可靠性

完成：

- 排队成功后再 ACK received。
- attempt 回传。
- running ACK。
- cmd_id 去重。
- 终态 ACK 重放。
- POST `/v3/tts`。
- generation stop。
- 固定播放缓冲。
- 删除 legacy ACK 和 GET TTS。

验收：

- SpeechQueue 满时无假 received。
- Server 重发同一 cmd_id 不重复播放。
- ACK POST 暂时失败后可补发。
- 重启后非幂等命令不重复执行。
- stop 可立即停止并清除旧语音。
- TTS 长文本和中文特殊字符不受 URL 限制。

### P7：清理和产品验收

完成：

- 删除 OpenClaw 配置。
- 删除无用模块。
- 更新 README、部署和运维文档。
- 增加 health 指标。
- 完成长稳测试。

验收：

- 代码搜索不再出现生产 OpenClaw 配置。
- 代码搜索不再出现发送 `device_result`。
- 代码搜索不再出现自定义 `/morrow/dialogue/stream`。
- 业务工具只在 Morrow 日志中执行。
- Server 不包含飞书、天气和路线业务调用。

---

## 18. 测试计划

### 18.1 Morrow 协议单元测试

覆盖：

- URL 构造。
- `start_turn` 严格 JSON。
- `cancel_turn` 严格 JSON。
- snapshot。
- text_delta。
- agent_message。
- robot_notice。
- turn_saved。
- turn_rejected。
- error。
- 未知 agent_event。
- 无 request_id 事件归属唯一 active turn。

### 18.2 coordinator 测试

覆盖：

- 单并发。
- FIFO。
- 队列满。
- 请求 TTL。
- turn 超时。
- stop generation。
- 断线中止。
- 不自动重放写操作。
- delta 后 final 不重复。
- 无 delta 使用 final。
- saved flush tail。

### 18.3 notice 测试

覆盖：

- 同 ID 去重。
- kind TTL。
- 离线等待。
- 过期。
- rendered ACK。
- failed ACK。
- Server 重启恢复。
- 未知 kind。
- notice 队列压力。

### 18.4 命令测试

覆盖：

- lease。
- attempt。
- received。
- running。
- rendered。
- retry。
- terminal monotonicity。
- 同 turn segment 顺序。
- 不同 source 去重。
- stop 取消未 lease 命令。
- payload retention。

### 18.5 固件测试

覆盖：

- 队列满。
- 重复 cmd_id。
- 重复 attempt。
- ACK 断网。
- 设备重启。
- TTS 中断。
- TTS HTTP 断流。
- stop。
- deadline 过期。
- UTF-8 边界。
- DJI 插拔。
- 内置麦克风回退。
- camera 与 TTS 并发资源压力。

### 18.6 业务验收

#### 上周工作复盘

输入：

```text
请复盘一下上周工作进度
```

预期：

- Morrow 调用 `lark_calendar_list`。
- Server 只收到文本。
- 机器人流式播报。
- Server 不直接访问飞书。

#### 创建飞书日程

输入：

```text
明天上午10点到11点的项目会，帮我建一个飞书日程，邀请张三、李四参会
```

预期：

- Morrow 调用联系人和日程工具。
- 只创建一次。
- Morrow 连接中断后 Server 不自动重放该 prompt。
- 机器人播报最终确认。

#### 天气

输入：

```text
今天杭州天气如何？
```

预期：

- Morrow 工具事件出现 `qweather_weather_query`。
- Xiaopai Server 不调用天气 API。
- 文本按句播报。

#### 路线

输入：

```text
从深圳福田区到深圳湾科技生态园开车大概多久？
```

预期：

- Morrow 工具事件出现 `amap_route_plan`。
- Xiaopai Server 不调用高德。
- 文本按句播报。

#### 会议提醒

条件：

- Morrow 默认 session 在线。
- 飞书存在满足规则的临近会议。

预期：

- Server 收到 `robot_notice`。
- notice ID 原样保存。
- 设备只播放一次。
- rendered ACK 后状态完成。

#### 外勤和出差提醒

预期：

- Morrow 自行调用路线和天气工具。
- Server 只消费 notice text。
- 不在 Xiaopai Server 重复实现规则。

---

## 19. 可观测性

health 至少返回：

```json
{
  "morrow": {
    "base_url": "http://127.0.0.1:3000",
    "session": "default",
    "connected": true,
    "snapshot_received": true,
    "active_request_id": "",
    "queued_turns": 0,
    "last_message_at": "",
    "last_notice_at": "",
    "last_error": ""
  },
  "commands": {
    "queued": 0,
    "leased": 0,
    "running": 0,
    "expired": 0
  },
  "devices": {
    "online": 1
  }
}
```

指标：

```text
morrow_ws_reconnect_total
morrow_turn_submitted_total
morrow_turn_rejected_total
morrow_turn_timeout_total
morrow_first_delta_latency_ms
morrow_first_speech_command_latency_ms
morrow_notice_received_total
morrow_notice_duplicate_total
morrow_notice_rendered_total
morrow_notice_expired_total
device_command_retry_total
device_ack_replay_total
speech_queue_full_total
```

日志必须带：

```text
request_id
notice_id
cmd_id
device_id
attempt
segment_index
generation
```

禁止把 `request_id`、Xiaopai `turn_id` 和 Morrow `turn_id` 混用。

---

## 20. 风险与边界

### 20.1 当前 Morrow 无设备交付反馈

Morrow 不知道机器人是否真正播放完成。

本版本的设备完成状态只在 Xiaopai Server 保存。不能声称 Morrow 会基于 `rendered` 自动切换备用飞书通知。

若未来确实需要这一能力，应由 Morrow 官方增加受支持的外部事件输入协议，再更新方案。不能由 Xiaopai 自行发送未定义消息。

### 20.2 当前 Morrow 无主动提醒历史补拉

WebSocket 断线期间可能漏 notice。

解决方向只能是：

- 提高连接稳定性。
- Morrow 官方增加 notice history 或 durable delivery。
- 将 Xiaopai Server 与 Morrow 部署在同一主机或同一局域网。
- 对 WebSocket 断线设置高优先级告警。

### 20.3 当前 Morrow 无结构化动作输出

不能让 Server 从回复文本猜动作。

本版本动作来源只允许：

- 本地状态机。
- 明确的人工调试命令。
- 固定 UI 映射。
- 后续 Morrow 官方支持的结构化接口。

### 20.4 单 default session

本版本适用于一个主要用户和一台主机器人。

多设备时可以让多个设备共享同一 Morrow 对话，但会造成上下文和回复路由问题，因此第一阶段不支持多个设备同时发起业务 turn。

### 20.5 ASR 与 TTS 不是第二套 Agent

ASR 只把声音转成文本。TTS 只把文本转成声音。它们不做业务决策，不违反“只使用 Morrow”。

---

## 21. 最终完成定义

只有同时满足以下条件，才认为迁移完成：

1. Morrow 是唯一业务 Agent。
2. Morrow 以 `server --robot` 运行。
3. Server 固定连接 `default` session。
4. Server 只使用 Morrow 已公开的 WebSocket 消息。
5. 不发送 `conversation_id` 和 `device_result`。
6. 同一时刻只运行一个 Morrow turn。
7. `text_delta` 可直接按标点进入 TTS。
8. `agent_message` 不重复播报。
9. `robot_notice` 使用 Morrow 原始 ID 去重。
10. notice 只有在设备 `rendered` 后才本地完成。
11. 设备命令只有一个生产控制通道。
12. 固件成功入队后才 ACK `received`。
13. 固件回传真实 `attempt` 和执行 tick。
14. 固件支持 cmd_id 去重和终态 ACK 重放。
15. 固件使用 `POST /v3/tts`。
16. Server 不直接调用飞书、天气和路线服务。
17. 生产代码中不再保留 OpenClaw 模型兼容配置。
18. 断线后不自动重放可能产生副作用的 prompt。
19. 业务验收覆盖复盘、建日程、天气、路线和三类主动提醒。
20. 完成至少 7 天连续运行测试，期间无重复日程创建、无重复提醒播报、无命令双通道重复执行。

---

## 22. 推荐最终目录

```text
stack-chan/
  stack-chan-server/
    src/
      server.py
      app_state.py
      morrow_protocol.py
      morrow_client.py
      morrow_coordinator.py
      notice_dispatcher.py
      database.py
      command_store.py
      command_dispatcher.py
      realtime_server.py
      asr_service.py
      tts_service.py
      vision_service.py
      ota_service.py
      schemas.py
    tests/
      test_morrow_protocol.py
      test_morrow_client.py
      test_morrow_coordinator.py
      test_morrow_notice.py
      test_command_store.py
      test_realtime_audio.py
      test_tts.py
  main/
    main_command_services.inc
    main_tts_commands.inc
    main_realtime_transport.inc
    main_realtime_speech.inc
    main_app_state.inc
    command_dedupe.cpp
    command_dedupe.h
    ack_replay.cpp
    ack_replay.h
```

本目录不是要求一次性机械拆分所有文件。优先级是先纠正协议边界和单 turn 行为，再逐步拆分当前过大的 `server.py`。
