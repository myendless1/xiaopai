# 出行规划

## 职责
为外出拜访和次日出差事件生成路线、出发时间、天气和准备提醒。

## 外出事件逻辑
1. 读取 scheduler 传入的 `calendar_event`。
2. 从 location、description、title 保守解析目的地。
3. 读取用户 profile，合并默认出发地、交通方式和提前到达 buffer。
4. 有出发地时调用 route adapter，计算建议出发时间。
5. 生成出行 speech 和 `current_focus`。

## 次日出差逻辑
1. 解析目的地。
2. 调用 weather adapter 查询目标日期天气。
3. 生成天气与准备事项提示。
4. 天气失败时降级，但仍给出确定性准备建议。

## 关键实现
- `openclaw-skills/plugins/work-assistant/src/travel/assistant.ts`: 外出/出差处理、目的地解析、路线和天气动作。
- `openclaw-skills/plugins/work-assistant/src/travel/adapters.ts`: Route、Weather、UserProfile 接口。
- `openclaw-skills/plugins/work-assistant/src/travel/dry-run.ts`: dry-run 路线、天气和 profile。
- `openclaw-skills/plugins/work-assistant/src/travel/qweather.ts`: QWeather 天气适配器。

## 注意点
- 非 dry-run 下路线 adapter 当前默认不可用，除非后续接入真实 provider。
- 缺少目的地时返回 failed `travel.plan.generate`。
- 缺少出发地时跳过路线，但仍生成有用提醒。
