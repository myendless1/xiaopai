import { readCredential } from "./config.js";
import type { WeatherCurrentRequest, WeatherForecastRequest, WeatherProviderConfig } from "./contracts.js";
import { QWeatherClient } from "./qweather.js";

export type WeatherProviderHandlerOptions = {
  config: WeatherProviderConfig;
  credential?: string;
  fetchImpl?: typeof fetch;
};

export function createWeatherProviderHandler(options: WeatherProviderHandlerOptions) {
  const credential = options.credential ?? readCredential(options.config);
  const client = new QWeatherClient({
    config: options.config,
    ...(credential ? { credential } : {}),
    ...(options.fetchImpl ? { fetchImpl: options.fetchImpl } : {})
  });
  return {
    getCurrent(request: WeatherCurrentRequest) {
      return client.getCurrent(request);
    },
    getForecast(request: WeatherForecastRequest) {
      return client.getForecast(request);
    }
  };
}
