# weather-provider

OpenClaw weather provider plugin backed by QWeather.

It exposes:

- Gateway methods: `weather.getCurrent`, `weather.getForecast`
- Tool aliases: `tool.weather.getCurrent`, `tool.weather.getForecast`
- Agent tools: `weather_current`, `weather_forecast`

Configure `apiHost` from QWeather Console and put the credential in the configured environment variable.
