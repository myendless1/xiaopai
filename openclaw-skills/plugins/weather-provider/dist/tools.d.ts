import type { AnyAgentTool } from "openclaw/plugin-sdk/plugin-entry";
import type { WeatherProviderHandlerOptions } from "./handler.js";
export declare function createWeatherForecastTool(options: WeatherProviderHandlerOptions): AnyAgentTool;
export declare function createWeatherCurrentTool(options: WeatherProviderHandlerOptions): AnyAgentTool;
