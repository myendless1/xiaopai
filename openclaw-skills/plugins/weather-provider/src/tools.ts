import { Type } from "typebox";
import type { AnyAgentTool } from "openclaw/plugin-sdk/plugin-entry";
import type { WeatherProviderHandlerOptions } from "./handler.js";
import { createWeatherProviderHandler } from "./handler.js";
import { isForecastDays } from "./qweather.js";

const ForecastSchema = Type.Object(
  {
    location: Type.String({ description: "City or place name, for example 北京 or 上海." }),
    adm: Type.Optional(Type.String({ description: "Optional superior administrative area used to disambiguate location lookup." })),
    range: Type.Optional(Type.String({ description: "Optional QWeather city lookup range, for example cn." })),
    days: Type.Optional(Type.Number({ description: "Forecast horizon: 3, 7, 10, 15, or 30.", enum: [3, 7, 10, 15, 30] })),
    date: Type.Optional(Type.String({ description: "Optional target date in YYYY-MM-DD. The tool returns selected forecast when available." }))
  },
  { additionalProperties: false }
);

const CurrentSchema = Type.Object(
  {
    location: Type.String({ description: "City or place name, for example 北京 or 上海." }),
    adm: Type.Optional(Type.String({ description: "Optional superior administrative area used to disambiguate location lookup." })),
    range: Type.Optional(Type.String({ description: "Optional QWeather city lookup range, for example cn." }))
  },
  { additionalProperties: false }
);

type ForecastParams = {
  location: string;
  adm?: string;
  range?: string;
  days?: number;
  date?: string;
};

type CurrentParams = {
  location: string;
  adm?: string;
  range?: string;
};

export function createWeatherForecastTool(options: WeatherProviderHandlerOptions): AnyAgentTool {
  return {
    name: "weather_forecast",
    label: "Weather Forecast",
    description: "Look up QWeather daily forecast for a city or place. Use this for weather questions about today, tomorrow, or upcoming travel.",
    parameters: ForecastSchema,
    execute: async (_toolCallId: string, params: ForecastParams) => {
      const handler = createWeatherProviderHandler(options);
      const request = {
        location: params.location,
        ...(params.adm ? { adm: params.adm } : {}),
        ...(params.range ? { range: params.range } : {}),
        ...(isForecastDays(params.days) ? { days: params.days } : {}),
        ...(params.date ? { date: params.date } : {})
      };
      return jsonToolResult(await handler.getForecast(request));
    }
  };
}

export function createWeatherCurrentTool(options: WeatherProviderHandlerOptions): AnyAgentTool {
  return {
    name: "weather_current",
    label: "Current Weather",
    description: "Look up QWeather current weather for a city or place.",
    parameters: CurrentSchema,
    execute: async (_toolCallId: string, params: CurrentParams) => {
      const handler = createWeatherProviderHandler(options);
      return jsonToolResult(
        await handler.getCurrent({
          location: params.location,
          ...(params.adm ? { adm: params.adm } : {}),
          ...(params.range ? { range: params.range } : {})
        })
      );
    }
  };
}

function jsonToolResult(value: unknown) {
  return {
    details: value,
    content: [
      {
        type: "text" as const,
        text: JSON.stringify(value, null, 2)
      }
    ]
  };
}
