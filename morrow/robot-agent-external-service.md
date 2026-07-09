# 机器人特别版外部服务接入说明

本文说明外部服务如何接入机器人特别版 Morrow Agent，如何通过 WebSocket 调用 agent，如何接收主动提醒，以及如何让 agent 长期在线。

当前机器人特别版发布版本建议使用：

```text
v0.1.2-robot.2
```

这个版本在机器人模式中已经包含飞书日程工具、和风天气查询工具、高德地图路线规划工具、后台主动提醒调度器和 WebSocket 会话重置能力。

## 1. 整体架构

推荐架构如下：

```text
外部服务
  通过 WebSocket 调用和监听
Morrow Robot Agent
  读取飞书日程，调用模型和工具，产生纯文本回复或主动提醒
飞书、和风天气、高德地图
  由 Morrow 统一访问
```

外部服务不需要直接调用 `lark-cli`、和风天气或高德地图。外部服务只需要连接 Morrow 的 HTTP 和 WebSocket 接口，把 Morrow 当作一个长期运行的 agent runtime。

注意：天气和高德地图工具只在 `morrow server --robot` 模式下暴露。直接执行 `morrow "今天杭州天气如何"` 属于普通 CLI 模式，只会加载基础文件和命令工具，不会加载机器人特别版工具。

## 2. 安装和更新（已经安装）

macOS 和 Linux 推荐用安装脚本安装指定机器人版本：

```bash
MORROW_VERSION=v0.1.2-robot.2 curl -fsSL https://raw.githubusercontent.com/catDforD/morrow/main/install.sh | sh
```

如果之前安装到了自定义目录，例如 `/usr/local/bin`，更新时要保持相同安装目录：

```bash
MORROW_VERSION=v0.1.2-robot.2 MORROW_INSTALL_DIR=/usr/local/bin curl -fsSL https://raw.githubusercontent.com/catDforD/morrow/main/install.sh | sh
```

安装后检查当前二进制路径：

```bash
which morrow
```

确认二进制里包含机器人天气和路线工具名：

```bash
strings "$(which morrow)" | grep -E "qweather_weather_query|amap_route_plan"
```

如果能看到 `qweather_weather_query` 和 `amap_route_plan`，说明安装的二进制已经包含新工具。

## 3. 启动 agent（已经启动，端口在 3000）

在已安装和配置好的机器上启动机器人模式：

```bash
morrow server --robot --host 0.0.0.0 --port 3000
```

默认监听地址：

```text
http://127.0.0.1:3000
```

启动前建议检查本地配置：

```bash
morrow robot doctor
```

预期结果应该是：

```text
robot doctor: ok
```

`robot doctor` 会检查 `lark-cli`、飞书权限、和风天气 token 配置和高德 key 配置。它主要确认配置是否存在和飞书基础权限是否可用；天气和路线接口的实际效果，建议通过一次真实对话触发工具来验证。

如果要使用指定会话作为默认会话，可以这样启动：

```bash
morrow --session robot-user-001 server --robot --host 0.0.0.0 --port 3000
```

注意：主动提醒会广播到 server 启动时的默认 session。默认不指定时是 `default`。

如果更新了二进制，必须重启正在运行的机器人进程。否则旧进程仍然使用旧版本。

systemd 示例：

```bash
sudo systemctl restart morrow-robot
sudo journalctl -u morrow-robot -f
```

手动启动示例：

```bash
pkill morrow
morrow server --robot --host 0.0.0.0 --port 3000
```

## 4. 会话和连接地址（从这一步开始看对接）

默认会话名是，目前如果不指定 session 名，会默认使用 `default`这个 session：

```text
default
```

默认 WebSocket 地址：

```text
ws://127.0.0.1:3000/api/sessions/default/ws
```

如果想使用指定会话，例如 `robot-user-001`，WebSocket 地址是：

```text
ws://127.0.0.1:3000/api/sessions/robot-user-001/ws
```

同一个 session 的所有连接共享同一份会话记录。外部服务如果需要多用户隔离，应该为每个用户使用不同 session。

## 5. HTTP 接口

### 5.1 健康和状态

```bash
curl http://127.0.0.1:3000/api/status
```

返回示例：

```json
{
  "workspace_root": "/home/gargantua/code/morrow",
  "config_path": "/home/gargantua/code/morrow/morrow.toml",
  "permissions": {
    "mode": "read_only",
    "shell": "prompt"
  },
  "version": "0.1.2"
}
```

`version` 显示的是 Rust workspace 版本，例如 `0.1.2`。机器人特殊发布号以 GitHub Release tag 为准，例如 `v0.1.2-robot.2`。

### 5.2 列出会话

```bash
curl http://127.0.0.1:3000/api/sessions
```

### 5.3 读取某个会话

```bash
curl http://127.0.0.1:3000/api/sessions/default
```

### 5.4 创建会话

```bash
curl -X POST http://127.0.0.1:3000/api/sessions/robot-user-001
```

如果会话已经存在，会返回 conflict。

### 5.5 重置会话

HTTP 方式：

```bash
curl -X POST http://127.0.0.1:3000/api/sessions/default/reset
```

也可以通过 WebSocket 重置，见下文。

## 6. WebSocket 协议

WebSocket 是外部服务接入的主要方式。外部服务通过同一个连接发送用户请求，也通过同一个连接接收模型输出、工具事件、主动提醒和会话快照。

所有消息都是 JSON。消息顶层使用：

```json
{
  "type": "message_type",
  "data": {}
}
```

## 7. 客户端发给 agent 的消息

### 7.1 发起一轮对话

```json
{
  "type": "start_turn",
  "data": {
    "request_id": "req-001",
    "prompt": "请复盘一下上周工作进度"
  }
}
```

字段说明：

`request_id` 是外部服务生成的幂等标识，建议使用 UUID。

`prompt` 是用户输入的自然语言内容。

常见 prompt 示例：

```text
请复盘一下上周工作进度
```

```text
明天上午10点到11点的项目会，帮我建一个飞书日程，邀请张三、李四参会
```

```text
我会晚到五分钟
```

```text
今天杭州天气如何？
```

```text
从深圳福田区到深圳湾科技生态园开车大概多久？
```

```text
明天去广州出差，帮我看一下天气和穿搭建议
```

### 7.2 重置当前会话

```json
{
  "type": "reset_session",
  "data": {
    "request_id": "reset-001"
  }
}
```

成功后，server 会广播一个新的 `snapshot`，其中 session 为空。

如果当前会话有正在运行的 turn，server 不会重置，会返回：

```json
{
  "type": "turn_rejected",
  "data": {
    "request_id": "reset-001",
    "reason": "session has a running turn"
  }
}
```

### 7.3 取消正在运行的 turn

```json
{
  "type": "cancel_turn",
  "data": {
    "turn_id": "turn-id-from-snapshot"
  }
}
```

### 7.4 审批工具调用（我给了全部权限，这点不用管）

当前机器人模式一般建议用只读或受控工具，外部服务通常不需要处理审批。但如果 server 产生了审批请求，可以发送：

```json
{
  "type": "approval_decision",
  "data": {
    "request_id": "approval-request-id",
    "approved": true
  }
}
```

## 8. agent 发给外部服务的消息

### 8.1 snapshot

连接建立后，server 会先发送当前会话快照：

```json
{
  "type": "snapshot",
  "data": {
    "session": {
      "active_thread": {
        "messages": []
      },
      "turns": [],
      "context": {
        "summarized_turns": 0
      }
    },
    "running_turn": null,
    "permissions": {
      "mode": "read_only",
      "shell": "prompt"
    }
  }
}
```

外部服务可以用它恢复当前会话状态。

### 8.2 agent_event（这里是你主要需要消费的内容，如果是做流式输出也可以用到，而且体感会更快一些）

模型输出、工具调用和 turn 生命周期都会通过 `agent_event` 返回。

外部服务做 TTS 时，主要关注两类事件：

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

```json
{
  "type": "agent_event",
  "data": {
    "event": {
      "type": "agent_message",
      "data": "上周你的主要工作集中在项目会议、客户沟通和交付准备。"
    }
  }
}
```

建议外部服务优先消费 `agent_message` 作为最终播报文本。如果需要实时流式语音，可以消费 `text_delta`。

工具调用事件也会通过 `agent_event` 返回。验证天气和高德工具是否生效时，可以关注工具名：

```text
qweather_weather_query
amap_route_plan
```

如果用户问天气或路线，但工具事件里没有出现这两个名称，通常说明没有用 `server --robot` 模式启动，或者当前会话连接到了旧进程。

### 8.3 robot_notice （这个就是被动功能会用到的消息，也需要消费。注意，动提醒只会广播到启动 server 时的默认 session，也就是不给指定 session 名，默认使用 default 即可）。

主动提醒通过 `robot_notice` 推送：

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

`kind` 可能是：

```text
meeting_reminder
fieldwork_reminder
travel_reminder
```

外部服务做 TTS 时，直接把 `data.text` 交给 TTS。

### 8.4 turn_saved 

一轮对话保存完成：

```json
{
  "type": "turn_saved",
  "data": {
    "session": "default",
    "turn_index": 12
  }
}
```

### 8.5 turn_rejected 

请求被拒绝：

```json
{
  "type": "turn_rejected",
  "data": {
    "request_id": "req-001",
    "reason": "session has a running turn"
  }
}
```

### 8.6 error 

普通错误：

```json
{
  "type": "error",
  "data": {
    "message": "invalid websocket message"
  }
}
```

## 9. 模型可主动调用的工具 （目前配置的相关工具）

机器人模式下，模型可以主动调用以下业务工具：

```text
lark_calendar_list
lark_user_search
lark_calendar_create
lark_event_get
lark_event_attendees_list
lark_message_send
qweather_weather_query
amap_route_plan
```

### 9.1 和风天气工具（已经配置好）

工具名：

```text
qweather_weather_query
```

用途：

```text
查询城市、区县、地址或地点的天气预报。适用于天气、穿搭、外勤准备、出差准备等问题。
```

配置来源：

```toml
[qweather]
token = "你的和风天气 key"
base_url = "https://你的和风天气 API Host"
```

如果 `morrow.toml` 中没有配置 `token`，会读取 `QWEATHER_TOKEN` 环境变量。

### 9.2 高德地图路线工具（已经配置好）

工具名：

```text
amap_route_plan
```

用途：

```text
查询驾车路线，返回距离和预计耗时。适用于外勤、通勤、路线规划和出发提醒等问题。
```

配置来源：

```toml
[amap]
key = "你的高德地图 key"
base_url = "https://restapi.amap.com"
```

如果 `morrow.toml` 中没有配置 `key`，会读取 `AMAP_API_KEY` 环境变量。

当前高德工具只支持驾车路线，`mode` 固定为 `driving`。

## 10. 主动提醒触发规则（被动能力以及触发的要求）

agent 每 60 秒扫描未来 24 小时的飞书日程。

### 10.1 会议提醒

识别规则：

日程标题包含以下关键词之一：

```text
会、会议、同步、评审、周会、项目
```

触发规则：

默认在会议开始前 15 分钟和 5 分钟提醒。

输出示例：

```text
你有一个会议将在5分钟后开始，主题是项目会。请提前准备。
```

### 10.2 外勤提醒

识别规则：

日程标题或地点包含以下关键词之一：

```text
外勤、客户、拜访、现场、调研
```

触发规则：

默认在日程开始前 60 分钟提醒。

处理逻辑：

1. 目的地优先取日程地点。
2. 如果没有地点，则用日程标题作为目的地。
3. 默认出发地是 `深圳市福田区`。
4. 调用高德地图估算路程耗时。
5. 调用和风天气获取目的地天气。

输出示例：

```text
你待会有一个外勤日程，主题是客户拜访，地点是深圳市。从默认出发地过去预计需要13分钟。目的地天气是雷阵雨，26到31度。建议提前出发并检查随身物品。
```

### 10.3 次日异地出差提醒

识别规则：

日程标题或地点包含以下关键词之一：

```text
出差、差旅、航班、高铁
```

或者日程地点不包含：

```text
深圳
```

触发规则：

默认每天 18:00 检查明天的日程。如果明天有异地出差日程，则主动提醒。

输出示例：

```text
明天有异地出差安排，主题是上海出差，目的地是上海市。当地天气是小雨，25到30度。建议今晚准备证件、充电器、电脑、电源、差旅票据和换洗衣物。
```

## 11. 外部服务参考实现

下面是一个 Node.js 示例，演示如何连接、发送用户请求、接收最终回复和主动提醒。

```js
const session = 'default'
const ws = new WebSocket(`ws://127.0.0.1:3000/api/sessions/${session}/ws`)

ws.addEventListener('open', () => {
  ws.send(JSON.stringify({
    type: 'start_turn',
    data: {
      request_id: crypto.randomUUID(),
      prompt: '请复盘一下上周工作进度'
    }
  }))
})

ws.addEventListener('message', (event) => {
  const message = JSON.parse(event.data)

  if (message.type === 'robot_notice') {
    const text = message.data.text
    console.log('主动提醒:', text)
    // sendToTts(text)
    return
  }

  if (message.type === 'agent_event') {
    const agentEvent = message.data.event
    if (agentEvent.type === 'agent_message') {
      console.log('最终回复:', agentEvent.data)
      // sendToTts(agentEvent.data)
    }
  }

  if (message.type === 'turn_rejected') {
    console.error('请求被拒绝:', message.data.reason)
  }

  if (message.type === 'error') {
    console.error('错误:', message.data.message)
  }
})

ws.addEventListener('close', () => {
  console.log('连接断开，需要重连')
})
```

重置会话：

```js
ws.send(JSON.stringify({
  type: 'reset_session',
  data: {
    request_id: crypto.randomUUID()
  }
}))
```

## 12. 长期在线和保活（当前 agent 已经系统托管长期在线，可以不用管了，但是需要做自动重连）

生产环境不要依赖手动终端运行。推荐用 systemd、supervisor、Docker restart policy 或 Kubernetes Deployment。

systemd 示例：

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

启用：

```bash
sudo systemctl daemon-reload
sudo systemctl enable morrow-robot
sudo systemctl start morrow-robot
sudo systemctl status morrow-robot
```

外部服务也需要做 WebSocket 自动重连：

1. 启动时连接 WebSocket。
2. 收到 `robot_notice` 后立即处理。
3. 连接断开后指数退避重连。
4. 重连后等待新的 `snapshot`。
5. 对外部请求使用唯一 `request_id`。

## 13. 测试和验收

### 13.1 检查安装版本是否包含新工具

```bash
strings "$(which morrow)" | grep -E "qweather_weather_query|amap_route_plan"
```

能看到两个工具名，说明二进制包含新能力。

### 13.2 检查配置

```bash
morrow robot doctor
```

重点看：

```text
qweather_token: ok
amap_key: ok
```

### 13.3 启动机器人模式 

```bash
morrow server --robot --host 0.0.0.0 --port 3000
```

通过浏览器打开：

```text
http://服务器IP:3000
```

发送天气问题：

```text
今天杭州天气如何？
```

预期结果：

```text
Tools 面板出现 qweather_weather_query，最终回复是纯文字天气说明。
```

发送路线问题：

```text
从深圳福田区到深圳湾科技生态园开车大概多久？
```

预期结果：

```text
Tools 面板出现 amap_route_plan，最终回复是纯文字路线耗时说明。
```

### 13.4 通过 WebSocket 测试

连接：

```bash
websocat ws://127.0.0.1:3000/api/sessions/default/ws
```

发送天气测试：

```json
{"type":"start_turn","data":{"request_id":"weather-001","prompt":"今天杭州天气如何？"}}
```

发送路线测试：

```json
{"type":"start_turn","data":{"request_id":"route-001","prompt":"从深圳福田区到深圳湾科技生态园开车大概多久？"}}
```

预期会收到 `tool_call_started`、`tool_call_finished`、`agent_message` 和 `turn_saved`。

### 13.5 五个业务功能的触发方式

以下操作都需要先启动机器人模式：

```bash
morrow server --robot --host 0.0.0.0 --port 3000
```

可以通过 Web UI 输入示例文本，也可以通过 WebSocket 发送 `start_turn`。如果是主动提醒类功能，外部服务需要保持 WebSocket 长连接，等待 `robot_notice`。

#### 13.5.1 上周工作复盘

前置条件：

```text
飞书 user 身份已经授权日程读取权限，并且上周日程里有可复盘的事件。
```

在 Web UI 输入：

```text
请复盘一下上周工作进度
```

或通过 WebSocket 发送：

```json
{"type":"start_turn","data":{"request_id":"weekly-review-001","prompt":"请复盘一下上周工作进度"}}
```

预期现象：

```text
Tools 面板出现 lark_calendar_list。最终回复会说明这是基于上周一到上周日飞书日程的工作进度复盘，并用纯文字总结主要工作、会议沟通和可跟进事项。
```

#### 13.5.2 创建飞书日程

前置条件：

```text
飞书 user 身份已经授权日程创建权限和联系人搜索权限。被邀请人需要能通过姓名、邮箱或 open_id 搜到。
```

在 Web UI 输入：

```text
明天上午10点到11点的项目会，帮我建一个飞书日程，邀请张三、李四参会
```

或通过 WebSocket 发送：

```json
{"type":"start_turn","data":{"request_id":"calendar-create-001","prompt":"明天上午10点到11点的项目会，帮我建一个飞书日程，邀请张三、李四参会"}}
```

预期现象：

```text
Tools 面板先出现 lark_user_search，再出现 lark_calendar_create。创建成功后，agent 会用纯文字确认日程标题、时间和参会人。
```

注意：

```text
这个操作会真实创建飞书日程。测试时建议使用测试标题、测试参会人或测试日历。
```

#### 13.5.3 会议会前提醒和迟到代通知

前置条件：

```text
飞书日程里有即将开始的会议。会议标题包含“会、会议、同步、评审、周会、项目”之一。默认提醒窗口是开始前 15 分钟和 5 分钟。
```

操作方式：

```text
在飞书中创建一个测试会议，开始时间设置在当前时间 15 分钟后或 5 分钟后。标题可以写“测试项目会”。保持 Web UI 或外部服务 WebSocket 连接在线。
```

预期会收到 `robot_notice`：

```json
{
  "type": "robot_notice",
  "data": {
    "kind": "meeting_reminder",
    "text": "你有一个会议将在5分钟后开始，主题是测试项目会。请提前准备。"
  }
}
```

收到提醒后，在 Web UI 输入：

```text
我会晚到五分钟
```

或通过 WebSocket 发送：

```json
{"type":"start_turn","data":{"request_id":"meeting-delay-001","prompt":"我会晚到五分钟"}}
```

预期现象：

```text
agent 会查询最近一小时内即将开始的会议，读取会议详情和参会信息。如果会议有可用的会议群 chat_id，会调用 lark_message_send 发送纯文本迟到通知。如果没有会议群或权限不足，会用纯文字说明无法代通知的原因。
```

注意：

```text
当前飞书消息发送使用 [lark].message_identity 配置。你当前配置如果是 user，就需要具备 im:message.send_as_user 相关权限。
```

#### 13.5.4 外勤日程智能出行规划提醒

前置条件：

```text
飞书日程里有外勤类日程。标题或地点包含“外勤、客户、拜访、现场、调研”之一。默认外勤提醒时间是开始前 60 分钟。
```

操作方式：

```text
在飞书中创建一个测试日程，标题写“客户拜访”或“现场调研”，地点填写真实目的地，例如“深圳湾科技生态园”，开始时间设置在当前时间 60 分钟后。保持 WebSocket 连接在线。
```

预期会收到 `robot_notice`：

```json
{
  "type": "robot_notice",
  "data": {
    "kind": "fieldwork_reminder",
    "text": "你待会有一个外勤日程，主题是客户拜访，地点是深圳湾科技生态园。从默认出发地过去预计需要若干分钟。目的地天气是若干天气。建议提前出发并检查随身物品。"
  }
}
```

触发时 agent 会：

```text
读取飞书日程，默认从 [robot].default_origin 出发，调用高德地图估算路线耗时，调用和风天气查询目的地天气，然后输出纯文字提醒。
```

#### 13.5.5 次日异地出差穿搭和物品提醒

前置条件：

```text
飞书日程里明天有异地出差类日程。标题或地点包含“出差、差旅、航班、高铁”之一，或者日程地点不包含“深圳”。默认检查时间是 [robot].workday_end_time，通常是 18:00。
```

操作方式：

```text
在飞书中创建一个明天的测试日程，标题写“上海出差”或“广州高铁差旅”，地点填写“上海市”或“广州市”。保持 WebSocket 连接在线，并在 [robot].workday_end_time 对应时间等待提醒。
```

如果要马上测试，可以临时把 `morrow.toml` 里的检查时间改成当前时间之后 1 到 2 分钟。比如当前时间是 17:58，就先改成：

```toml
[robot]
workday_end_time = "18:00"
```

改完后重启服务：

```bash
sudo systemctl restart morrow-robot
```

预期会收到 `robot_notice`：

```json
{
  "type": "robot_notice",
  "data": {
    "kind": "travel_reminder",
    "text": "明天有异地出差安排，主题是上海出差，目的地是上海市。当地天气是若干天气。建议今晚准备证件、充电器、电脑、电源、差旅票据和换洗衣物。"
  }
}
```

注意：

```text
次日出差检查每天只触发一次。机器人会在本地状态里记录已经检查过的日期，避免重复提醒。
```

## 14. 重要限制（一般不适用普通 CLI ，所以不用太关心，而且普通 CLI 不支持机器人版能力）

### 14.1 普通 CLI 不加载机器人特别版工具

下面这种用法是普通 CLI 模式：

```bash
morrow "今天杭州天气如何？"
```

普通 CLI 只加载基础工具，不会加载 `qweather_weather_query` 和 `amap_route_plan`。机器人版能力必须通过：

```bash
morrow server --robot
```

再由 Web UI 或 WebSocket 调用。

### 14.2 主动提醒是实时广播

当前 `robot_notice` 是 WebSocket 实时广播。如果外部服务断线时刚好产生提醒，这条提醒可能会错过。

如果后续需要生产级可靠投递，建议增加 notice outbox：

1. agent 将 `robot_notice` 持久化。
2. 外部服务拉取未确认提醒。
3. 外部服务处理后 ack。
4. agent 根据 ack 清理或保留。

### 14.3 当前服务默认无鉴权

不要直接暴露到公网。

推荐部署方式：

```text
外部服务和 Morrow 部署在同一台机器或同一内网
```

如果必须远程访问，建议在前面加：

```text
VPN、mTLS、Nginx 鉴权、API Gateway、Cloudflare Access
```

### 14.4 同一个 session 同时只能跑一个 turn

如果某个 session 正在执行一轮对话，再发送新的 `start_turn` 可能会被拒绝。

外部服务应该：

1. 等待 `turn_saved` 后再发下一轮。
2. 或者为不同用户使用不同 session。
3. 必要时先发送 `cancel_turn`。

## 15. 推荐外部服务处理流程（可以按这个流程来处理消息和发送消息）

启动流程：

```text
1. 调用 /api/status 检查 agent 是否在线。
2. 连接 /api/sessions/{session}/ws。
3. 收到 snapshot 后恢复本地状态。
4. 进入长连接监听。
```

用户请求流程：

```text
1. 外部服务生成 request_id。
2. 发送 start_turn。
3. 监听 agent_event。
4. 收到 agent_message 后交给 TTS。
5. 收到 turn_saved 后标记请求完成。
```

主动提醒流程：

```text
1. 长连保持在线。
2. 收到 robot_notice。
3. 读取 data.text。
4. 交给 TTS 或消息推送。
5. 用 data.id 做外部侧去重。
```

重置流程：

```text
1. 发送 reset_session。
2. 收到 snapshot。
3. 确认 session.active_thread.messages 为空。
4. 清理外部服务侧缓存。
```

## 16. 最小可用接入清单

外部服务至少需要实现：

```text
WebSocket 长连接
断线重连
start_turn
robot_notice 消费
agent_message 消费
reset_session
turn_rejected 和 error 处理
```

TTS 侧只需要消费纯文本：

```text
robot_notice.data.text
agent_event.data.event.data，当 event.type 是 agent_message
```

