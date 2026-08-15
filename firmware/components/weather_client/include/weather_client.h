#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 设备直连 QWeather `/v7/weather/now`；失败仅降级天气页，结果经 sys_state 发布。
 * api_host 不含协议前缀；location 为城市 LocationID。 */
esp_err_t weather_client_init(const char *api_key, const char *api_host, const char *location);

#ifdef __cplusplus
}
#endif
