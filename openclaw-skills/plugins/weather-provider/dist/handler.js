import { readCredential } from "./config.js";
import { QWeatherClient } from "./qweather.js";
export function createWeatherProviderHandler(options) {
    const credential = options.credential ?? readCredential(options.config);
    const client = new QWeatherClient({
        config: options.config,
        ...(credential ? { credential } : {}),
        ...(options.fetchImpl ? { fetchImpl: options.fetchImpl } : {})
    });
    return {
        getCurrent(request) {
            return client.getCurrent(request);
        },
        getForecast(request) {
            return client.getForecast(request);
        }
    };
}
//# sourceMappingURL=handler.js.map