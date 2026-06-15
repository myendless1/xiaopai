# OpenClaw 能力开发设计方向

## 背景

当前需求文档以“工作日的一天”为演示场景，覆盖日程播报、日程创建、会议提醒、外勤出行、久坐关怀和出差提醒等能力。

openclaw 侧不建议按演示脚本硬编码 6 个固定流程，而应抽象为一组可复用的“工作助理能力”。机器人端主要负责唤醒、感知、屏幕展示、动作、灯光和语音播放；openclaw 侧负责业务理解、工具调用、决策和结构化结果输出。

## OpenClaw 侧职责边界

openclaw 主要承担以下职责：

1. 理解输入事件

   输入可能来自用户主动交互，也可能来自系统或机器人感知事件。

   示例：

   - 用户摸头
   - 唤醒词触发
   - 用户自然语言指令
   - 距离会议开始还有 5 分钟
   - 检测到用户久坐
   - 检测到次日有出差日程

2. 拉取和加工业务数据

   示例：

   - 飞书日历
   - 飞书通讯录
   - 会议室和参会人
   - 会议群
   - 日程备注
   - 天气
   - 路线和通勤耗时
   - 用户所在办公地点和个人偏好

3. 做业务决策和生成回复

   示例：

   - 今天有哪些重要事项需要提醒
   - 本周日程结构是否过载
   - 是否需要预留深度工作时间
   - 外勤应该几点出发
   - 出差需要携带哪些物品
   - 用户指令是否缺少必要信息，是否需要追问

4. 输出结构化结果给机器人端

   openclaw 不直接控制机器人硬件动作，而是输出可被机器人端消费的结构化结果。

   示例：

   ```json
   {
     "speech": "好的，已经为你创建好日程，并邀请了参会人。",
     "presentation": {
       "emotion": "thinking",
       "motion": "nod",
       "light": "blink"
     },
     "actions": [
       {
         "type": "lark.calendar.create",
         "status": "success",
         "resource_id": "calendar_event_id"
       }
     ],
     "follow_up": {
       "expected": false
     }
   }
   ```

## 推荐整体架构

建议采用事件驱动架构：

```text
InputEvent
  -> OpenClaw Intent Router
  -> Domain Skill
  -> Tool / Adapter
  -> Structured Response
```

### 输入事件类型

建议将输入统一建模为事件，而不是只处理文本。

用户主动事件：

- `head_touch`
- `wake_word`
- `user_utterance`
- `user_confirmation`

系统主动事件：

- `daily_briefing_triggered`
- `meeting_starting_soon`
- `outdoor_event_detected`
- `sedentary_detected`
- `business_trip_tomorrow_detected`

### 输出结果类型

建议所有能力统一返回结构化响应，至少包含：

- `speech`：机器人语音播报内容
- `presentation`：建议的表情、动作、灯光提示
- `actions`：openclaw 实际执行过的工具动作
- `follow_up`：是否期待用户继续补充信息
- `context`：需要保留的短期上下文

## 能力模块拆分

### 1. agenda_briefing：日程播报与工作复盘

对应需求文档中的“早上 9:30 日程管理及播报”。

核心能力：

- 查询上周日程。
- 对日程做分类统计，例如内部会议、客户接待、外出活动、深度工作。
- 生成上周工作复盘。
- 查询今日日程。
- 识别今日重点提醒，例如会议、客户接待、外出、会议室、准备材料。
- 生成适合语音播报的自然话术。

设计重点：

- 不只做简单列表播报，而要做“日程洞察”。
- 分类规则需要可配置，避免长期依赖关键词硬编码。
- 播报内容要控制长度，优先突出今天最重要的事项。

### 2. calendar_assistant：飞书日程创建助手

对应需求文档中的“早上 10:00 日程建立”。

核心能力：

- 从用户自然语言中解析日程标题、日期、开始时间、结束时间。
- 解析参会人姓名。
- 调用通讯录，将姓名解析为飞书用户 ID。
- 创建飞书日程。
- 邀请参会人。
- 对缺失或冲突信息发起追问。

设计重点：

- 日程创建属于写操作，需要更稳健的参数校验。
- 当参会人重名、时间不明确、缺少标题或时间冲突时，应先追问，不应盲目创建。
- 可以区分“低风险直接执行”和“高风险确认后执行”。

建议执行流程：

```text
用户指令
  -> 解析日程字段
  -> 校验必要参数
  -> 解析参会人
  -> 检查冲突或歧义
  -> 创建日程
  -> 返回结果
```

### 3. meeting_reminder_and_notify：会前提醒与临时通知

对应需求文档中的“早上 11:00 会议会前提醒及临时变动”。

核心能力：

- 在会议开始前 N 分钟触发提醒。
- 读取会议标题、时间、会议室、参会人。
- 生成会前提醒话术。
- 在用户说“晚五分钟到”等临时变动时，识别当前上下文中的会议。
- 找到会议群或参会人。
- 通过飞书发送通知消息。

设计重点：

- 需要维护短期上下文，例如“刚刚提醒的是哪场会议”。
- 用户的二次指令往往省略对象，例如“帮我跟参会人员说一下”，必须依赖上下文补全。
- 飞书消息发送应作为明确的 side effect 记录在响应中。

建议短期上下文示例：

```json
{
  "current_focus": {
    "type": "calendar_event",
    "event_id": "calendar_event_id",
    "title": "项目会",
    "start_time": "2026-06-06T11:00:00+08:00",
    "location": "中1会议室"
  }
}
```

### 4. travel_planner：外勤与出差出行建议

对应需求文档中的“下午 14:40 日常外勤出行规划”和“下午 18:30 异地出差出行建议”。

核心能力：

- 从日程中识别外勤、客户拜访、出差等行程。
- 提取目的地、城市、时间和备注。
- 查询路线耗时。
- 根据办公地点、目的地、预计路程和提前到达时间，计算建议出发时间。
- 查询目的地天气。
- 根据天气、温差、备注生成携带建议。
- 从日程备注中抽取额外注意事项，例如正装、材料、证件等。

设计重点：

- 当日外勤提醒重点是“几点出发”和“如何避免迟到”。
- 次日出差提醒重点是“天气、证件、办公资料、着装和备注事项”。
- 路线和天气属于外部依赖，应做好失败降级。例如查不到路线时，只播报日程和建议用户预留充足时间。

建议拆成两个触发场景：

- `outdoor_event_detected`：当日外勤提前提醒。
- `business_trip_tomorrow_detected`：前一日下班前出差提醒。

### 5. wellbeing_companion：久坐关怀与轻量陪伴

对应需求文档中的“下午 15:00 久坐关怀情绪陪伴”。

核心能力：

- 接收机器人端或感知服务发来的久坐事件。
- 判断当前是否适合打扰用户。
- 避开会议中、通话中、刚被提醒过等不适合场景。
- 生成久坐关怀话术。
- 用户接受后，提供笑话、轻量闲聊或放松建议。
- 结合近期日程，顺带提醒即将发生的事项。

设计重点：

- openclaw 不建议负责“每 20 分钟拍照识别”这类机器人感知逻辑。
- 机器人端或感知服务只需要发送结构化事件，例如：

  ```json
  {
    "type": "sedentary_detected",
    "duration_minutes": 30,
    "confidence": 0.86
  }
  ```

- openclaw 负责判断、组织话术和关联日程提醒。

## 关键数据模型建议

### InputEvent

```json
{
  "event_id": "evt_xxx",
  "type": "user_utterance",
  "timestamp": "2026-06-05T10:00:00+08:00",
  "user_id": "user_xxx",
  "payload": {
    "text": "明天上午10点到11点的项目会，帮我建一个飞书日程，邀请张三、李四参会。"
  },
  "context": {
    "locale": "zh-CN",
    "timezone": "Asia/Shanghai",
    "device_id": "robot_xxx"
  }
}
```

### StructuredResponse

```json
{
  "speech": "好的，已经为你创建好日程，并邀请了张三和李四参会。",
  "presentation": {
    "emotion": "smile",
    "motion": "nod",
    "light": "blink"
  },
  "actions": [
    {
      "type": "lark.calendar.create",
      "status": "success",
      "resource_id": "event_xxx"
    }
  ],
  "follow_up": {
    "expected": false,
    "question": null
  },
  "context_patch": {
    "last_created_calendar_event_id": "event_xxx"
  }
}
```

### ToolAction

```json
{
  "type": "lark.message.send",
  "input": {
    "chat_id": "chat_xxx",
    "content": "不好意思各位，张三将稍晚五分钟参会，请稍等一下。"
  },
  "status": "success",
  "created_at": "2026-06-05T10:55:00+08:00"
}
```

## 外部工具和适配器

初期可能需要以下 adapter：

- `LarkCalendarAdapter`：查询日程、创建日程、邀请参会人。
- `LarkContactAdapter`：按姓名解析用户。
- `LarkIMAdapter`：发送会议群或参会人消息。
- `WeatherAdapter`：查询目的地天气。
- `RouteAdapter`：查询路线耗时。
- `UserProfileAdapter`：读取用户办公地点、常用城市、偏好配置。
- `ContextStore`：保存短期上下文，例如最近提醒的会议。

## MVP 优先级

建议按以下顺序开发：

1. `calendar_assistant`

   原因：用户主动触发、输入输出明确、可以形成完整闭环，也是最容易验证价值的写操作能力。

2. `agenda_briefing`

   原因：能体现日程汇总和智能分析价值，适合作为每天早上的高频能力。

3. `meeting_reminder_and_notify`

   原因：打通主动提醒、上下文理解和飞书消息发送，是从“问答助手”升级为“主动助理”的关键能力。

4. `travel_planner`

   原因：外部依赖较多，需要接路线和天气，但用户感知明显，适合第二阶段增强。

5. `wellbeing_companion`

   原因：openclaw 侧难点不高，主要依赖机器人端或感知服务事件质量，适合最后接入。

## 需要提前确认的问题

后续进入详细设计前，建议确认以下问题：

- 机器人端和 openclaw 之间的事件协议是否已有标准。
- openclaw 当前是否已有 skill/plugin 机制，还是需要新建服务模块。
- 飞书日历、通讯录、IM 消息发送的权限边界是什么。
- 会议群如何定位：日程是否天然关联群聊，还是需要按参会人单独发消息。
- 日程分类规则由谁维护，是否需要业务侧可配置。
- 外勤目的地从日程标题、地点字段还是备注字段中提取。
- 路线服务和天气服务使用哪个供应商。
- 久坐检测由机器人端提供事件，还是 openclaw 需要接视觉识别服务。

## 总体结论

openclaw 应该作为“业务脑”和“工具编排层”来建设：理解事件，调用飞书、天气、路线等工具，生成决策和话术，再以结构化响应交给机器人端表达。

这样设计的好处是，后续新增场景时可以通过增加事件触发、领域能力和工具适配器来扩展，而不是不断堆叠固定演示脚本。
