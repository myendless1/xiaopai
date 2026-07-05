# 天气 Provider

## 职责
提供独立 OpenClaw 天气查询插件，底层使用 QWeather，供 agent 或业务插件查询当前天气和天气预报。

## 能力
- Gateway: `weather.getCurrent`、`weather.getForecast`
- Tool alias: `tool.weather.getCurrent`、`tool.weather.getForecast`
- Agent tools: `weather_current`、`weather_forecast`

## 关键实现
- `openclaw-skills/plugins/weather-provider/src/index.ts`: 插件注册、Gateway 方法和 agent tools。
- `openclaw-skills/plugins/weather-provider/src/config.ts`: 读取 `apiHost`、credential env、auth mode、默认语言、单位和 forecastDays。
- `openclaw-skills/plugins/weather-provider/src/qweather.ts`: Geo lookup、当前天气、逐日预报、错误规范化。
- `openclaw-skills/plugins/weather-provider/src/tools.ts`: TypeBox 参数 schema 和工具输出。

## 注意点
- 支持 `apiKeyHeader`、`apiKeyQuery`、`jwtBearer` 三种认证模式。
- 城市查询失败时会对部分已知地点做本地 fallback。
- 预报可选 `date`，命中时返回 `selected` 和摘要。
