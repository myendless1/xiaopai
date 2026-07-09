# DEMO_CMD

本文件中的命令默认发送到本机 Xiaopai server：

```bash
http://127.0.0.1:8091/command
```

说明：

- 不考虑灯光，不下发灯光或 `state` 指令。
- 本文件的 `speak` 示例都带 `animate_mouth:false`，说话时只播放声音，屏幕保持前一步设置的静态表情；要恢复动态嘴型可删除该字段或设为 `true`。
- 头部动作只保留两类：
  - 转向用户：使用 `find_owner`。
  - 点头：使用 `motion down/up/down` 低头、抬头、回正组合。
- `find_owner` 需要摄像头检测到人脸；如果没有检测到，当前固件会让后续 `sequence` 中断。

## 1. 晨间唤醒

- 屏幕表情：微笑表情
- 头部动作：find_owner
- 说话内容：早上好及日期播报

```bash
curl -X POST 'http://127.0.0.1:8091/command' \
  -H 'Content-Type: application/json' \
  -d '{
    "type": "sequence",
    "interrupt": true,
    "payload": [
      {"type":"face","expression":"smile_blink"},
      {"type":"find_owner","rounds":1,"speak":false,"preserve_speech":true},
      {"type":"speak","text":"早上好呀！今天是6月1日，周一，新的一周开始啦！","animate_mouth":false}
    ]
  }'
```

## 2. 周度复盘

- 屏幕表情：思考表情
- 头部动作：无
- 说话内容：上周日程统计及工作建议

```bash
curl -X POST 'http://127.0.0.1:8091/command' \
  -H 'Content-Type: application/json' \
  -d '{
    "type": "sequence",
    "interrupt": true,
    "payload": [
      {"type":"face","expression":"thinking"},
      {"type":"speak","text":"上周工作为您复盘，上周累计日程25项：其中内部会议占50%、客户接待占20%、外出活动占5%，会议占用时间偏高，深度工作时间偏少。建议本周预留整块时间用于项目攻坚与深度工作哦~","animate_mouth":false}
    ]
  }'
```

## 3. 今日日程汇总

- 屏幕表情：眨眼表情
- 头部动作：无
- 说话内容：当日会议、接待及外出提醒

```bash
curl -X POST 'http://127.0.0.1:8091/command' \
  -H 'Content-Type: application/json' \
  -d '{
    "type": "sequence",
    "interrupt": true,
    "payload": [
      {"type":"face","expression":"wink"},
      {"type":"speak","text":"同时为您汇总今日日程：今日共有2场内部会议、1场外部接待、1个外出，请不要迟到哦~","animate_mouth":false}
    ]
  }'
```

## 4. 语音助手唤醒

- 屏幕表情：眨眼表情
- 头部动作：find_owner
- 说话内容：“我在呢，请问有什么可以帮您？”

```bash
curl -X POST 'http://127.0.0.1:8091/command' \
  -H 'Content-Type: application/json' \
  -d '{
    "type": "sequence",
    "interrupt": true,
    "payload": [
      {"type":"face","expression":"wink"},
      {"type":"find_owner","rounds":1,"speak":false,"preserve_speech":true},
      {"type":"speak","text":"我在呢，请问有什么可以帮您？","animate_mouth":false}
    ]
  }'
```

## 5. 创建飞书日程

- 屏幕表情：思考表情
- 头部动作：无
- 说话内容：日程创建成功及参会人员邀请确认

```bash
curl -X POST 'http://127.0.0.1:8091/command' \
  -H 'Content-Type: application/json' \
  -d '{
    "type": "sequence",
    "interrupt": true,
    "payload": [
      {"type":"face","expression":"thinking"},
      {"type":"speak","text":"好的，已经为你创建好日历，并把参会人员拉进日程里啦","animate_mouth":false}
    ]
  }'
```

## 6. 用户致谢

- 屏幕表情：害羞表情
- 头部动作：无
- 说话内容：“不客气！举手之劳~”

```bash
curl -X POST 'http://127.0.0.1:8091/command' \
  -H 'Content-Type: application/json' \
  -d '{
    "type": "sequence",
    "interrupt": true,
    "payload": [
      {"type":"face","expression":"shy"},
      {"type":"speak","text":"不客气！举手之劳~","animate_mouth":false}
    ]
  }'
```

## 7. 会前提醒

- 屏幕表情：眨眼表情
- 头部动作：find_owner
- 说话内容：会议时间、地点及提前准备提醒

```bash
curl -X POST 'http://127.0.0.1:8091/command' \
  -H 'Content-Type: application/json' \
  -d '{
    "type": "sequence",
    "interrupt": true,
    "payload": [
      {"type":"face","expression":"wink"},
      {"type":"find_owner","rounds":1,"speak":false,"preserve_speech":true},
      {"type":"speak","text":"打扰一下，11:00的FC周例会还有五分钟将在中1会议室开始，请提前准备参会~","animate_mouth":false}
    ]
  }'
```

## 8. 代发迟到通知

- 屏幕表情：眨眼表情
- 头部动作：点头
- 说话内容：已通过飞书通知晚到五分钟

```bash
curl -X POST 'http://127.0.0.1:8091/command' \
  -H 'Content-Type: application/json' \
  -d '{
    "type": "sequence",
    "interrupt": true,
    "payload": [
      {"type":"face","expression":"wink"},
      {"type":"motion","action":"down","degree":10,"duration_ms":500},
      {"type":"motion","action":"up","degree":20,"duration_ms":500},
      {"type":"motion","action":"down","degree":10,"duration_ms":500},
      {"type":"speak","text":"没问题，我已经通过飞书替您通知将晚五分钟到场，请大家稍作等待。","animate_mouth":false}
    ]
  }'
```

## 9. 外勤出行提醒

- 屏幕表情：眨眼表情
- 头部动作：无
- 说话内容：外出地点、车程及建议出发时间

```bash
curl -X POST 'http://127.0.0.1:8091/command' \
  -H 'Content-Type: application/json' \
  -d '{
    "type": "sequence",
    "interrupt": true,
    "payload": [
      {"type":"face","expression":"wink"},
      {"type":"speak","text":"哈喽，您下午16:00有外出去福田会堂，驾车预计15分钟，建议15:40出发，可提前5分钟到达目的地。","animate_mouth":false}
    ]
  }'
```

## 10. 久坐提醒

- 屏幕表情：轻松表情
- 头部动作：无
- 说话内容：久坐提示及拉伸、远眺建议

```bash
curl -X POST 'http://127.0.0.1:8091/command' \
  -H 'Content-Type: application/json' \
  -d '{
    "type": "sequence",
    "interrupt": true,
    "payload": [
      {"type":"face","expression":"relaxed"},
      {"type":"speak","text":"我观察到您下午已经久坐30分钟啦，身体需要短暂放松。建议起身活动拉伸一下、眺望远方舒缓眼睛疲劳。","animate_mouth":false}
    ]
  }'
```

## 11. 笑话邀请

- 屏幕表情：微笑表情
- 头部动作：无
- 说话内容：“要不要给你讲一个笑话？”

```bash
curl -X POST 'http://127.0.0.1:8091/command' \
  -H 'Content-Type: application/json' \
  -d '{
    "type": "sequence",
    "interrupt": true,
    "payload": [
      {"type":"face","expression":"smile_blink"},
      {"type":"speak","text":"要不要给你讲一个笑话，放松一下？","animate_mouth":false}
    ]
  }'
```

## 12. 讲笑话

- 屏幕表情：开心表情
- 头部动作：点头
- 说话内容：虾和蚌的谐音笑话

```bash
curl -X POST 'http://127.0.0.1:8091/command' \
  -H 'Content-Type: application/json' \
  -d '{
    "type": "sequence",
    "interrupt": true,
    "payload": [
      {"type":"face","expression":"happy_squint"},
      {"type":"motion","action":"down","degree":10,"duration_ms":500},
      {"type":"motion","action":"up","degree":20,"duration_ms":500},
      {"type":"motion","action":"down","degree":10,"duration_ms":500},
      {"type":"speak","text":"虾和蚌同时考了一百分，老师问虾：你抄的谁的。虾说：我抄蚌，超棒的。","animate_mouth":false}
    ]
  }'
```

## 13. 出差天气提醒

- 屏幕表情：思考表情
- 头部动作：find_owner
- 说话内容：上海出差时间、气温及降雨提醒

```bash
curl -X POST 'http://127.0.0.1:8091/command' \
  -H 'Content-Type: application/json' \
  -d '{
    "type": "sequence",
    "interrupt": true,
    "payload": [
      {"type":"face","expression":"thinking"},
      {"type":"find_owner","rounds":1,"speak":false,"preserve_speech":true},
      {"type":"speak","text":"哈喽，准备下班咯~提醒您明早9点将前往上海出差，上海明日25℃，有80%概率降雨，与深圳温差较大哦~","animate_mouth":false}
    ]
  }'
```

## 14. 出差物品提醒

- 屏幕表情：开心表情
- 头部动作：点头
- 说话内容：外套、雨具、身份证、充电器及资料提醒

```bash
curl -X POST 'http://127.0.0.1:8091/command' \
  -H 'Content-Type: application/json' \
  -d '{
    "type": "sequence",
    "interrupt": true,
    "payload": [
      {"type":"face","expression":"happy_squint"},
      {"type":"motion","action":"down","degree":10,"duration_ms":500},
      {"type":"motion","action":"up","degree":20,"duration_ms":500},
      {"type":"motion","action":"down","degree":10,"duration_ms":500},
      {"type":"speak","text":"记得增添外套，带好雨具。同时提醒您记得携带身份证、充电器、出差办公资料~","animate_mouth":false}
    ]
  }'
```

## 15. 出差收尾提醒

- 屏幕表情：比心表情
- 头部动作：无
- 说话内容：正装提醒及一路平安祝福

```bash
curl -X POST 'http://127.0.0.1:8091/command' \
  -H 'Content-Type: application/json' \
  -d '{
    "type": "sequence",
    "interrupt": true,
    "payload": [
      {"type":"face","expression":"heart_action"},
      {"type":"speak","text":"查询到日程备注栏写着建议着正装，建议您带上一套，祝您一路平安！回来后见~","animate_mouth":false}
    ]
  }'
```
