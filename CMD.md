飞书发消息 API （由于飞书限制跨租户 lark-cli 发消息，只能发群聊，然后我整理了一下能发的一些群聊）

命令：
lark-cli im +messages-send \
    --chat-id oc_806c21b8cdcff6bb4adb0b734027d794 \
    --text "测试消息" \
    --as user

可用群聊信息：
项目群
     chat_id: oc_af0c9dfb1f000e29e5ffed6715db510c
     external: false

🤖CreAIthon小队
     chat_id: oc_806c21b8cdcff6bb4adb0b734027d794
     external: true

只需要将命令中的 chat_id 替换一下就行了，text 中就是发的内容，我账号上是能正常在群聊里收到消息的。

创建日程 API

命令：
 lark-cli calendar +create \
    --summary "项目会会议" \
    --as user \
    --

  常用参数：
  --summary        日程标题
  --start          开始时间，ISO 8601 格式
  --end            结束时间，ISO 8601 格式
  --description    日程描述，可选
  --attendee-ids   参会人 ID，逗号分隔，可选
  --calendar-id    指定日历 ID，可选；不填默认主日历
  --rrule          重复规则，可选
  --dry-run        只预览请求，不真正创建
  --as user        用当前用户身份创建

示例，创建带参会人的日程：
lark-cli calendar +create \
    --summary "项目沟通" \
    --description "同步项目进展和风险" \
    --start "2026-06-23T14:00:00+08:00" \
    --end "2026-06-23T15:00:00+08:00" \
    --attendee-ids ou_xxx,ou_yyy \
    --as user

当前可邀请的用户：
  邓鑫宇
  ou_aea109214d3521168b403d875ffe1145

  庞静怡
  ou_b821e2b10f41077ed3026562b09f7a56

  Ying LI 李颖
  ou_815445631faa540adc178cc16970b636

  Gargantua
  ou_b568e3da5cfebd80e171bb71f3aa63a8


lark-cli calendar +create \
  --summary "项目会会议" \
  --as user \
  --start "2026-06-24T16:00+08:00" \
  --end "2026-06-24T17:00+08:00" \
  --attendee-ids "ou_aea109214d3521168b403d875ffe1145,ou_b821e2b10f41077ed3026562b09f7a56,ou_815445631faa540adc178cc16970b636"

lark-cli im +messages-send \
  --chat-id oc_af0c9dfb1f000e29e5ffed6715db510c \
  --text "@Ying LI 李颖 主人让我提醒你他要要晚5分钟到" \
  --as user