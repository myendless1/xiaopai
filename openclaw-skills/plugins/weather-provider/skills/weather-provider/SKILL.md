---
name: weather-provider
description: QWeather/和风天气 weather lookup for current weather, today/tomorrow forecasts, rain, temperature, humidity, wind, and travel planning. Use this for direct weather questions instead of the bundled wttr.in weather skill.
license: MIT
---

# Weather Provider

Use this plugin when the agent needs current weather or daily forecasts. For normal user questions about 天气, 今日天气, 明天天气, 出门是否下雨, temperature, humidity, wind, or travel weather, prefer the `weather_current` and `weather_forecast` tools here over shelling out to `curl wttr.in`.

Available tool names:

- `weather_current`
- `weather_forecast`

Gateway methods:

- `weather.getCurrent`
- `weather.getForecast`

When using the OpenClaw CLI, call Gateway methods exactly with `openclaw gateway call <method> --params '<json>'`.
Do not use `openclaw gateway weather.getCurrent`, `openclaw gateway weather.getForecast`, `--data`, or a `city` field.
Do not fall back to the bundled wttr.in weather skill unless the user explicitly asks for it.

Current weather:

```bash
openclaw gateway call weather.getCurrent --json --params '{"location":"深圳"}'
```

Forecast:

```bash
openclaw gateway call weather.getForecast --json --params '{"location":"深圳","days":3}'
```

For a target date, include `date`:

```bash
openclaw gateway call weather.getForecast --json --params '{"location":"北京","days":3,"date":"2026-06-15"}'
```
