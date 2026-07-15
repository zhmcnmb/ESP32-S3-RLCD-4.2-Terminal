#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 天气客户端: 设备直连和风天气(QWeather) `/v7/weather/now`，展示当前实况。
 *
 * 直连而非走Mac桥接代理: QWeather新版API Host强制gzip压缩响应，需要TLS+gzip解压。
 * TLS走ESP官方证书bundle(cloud_transport组件)，gzip解压复用ESP32 ROM自带的miniz
 * tinfl函数(零额外代码体积)。详见 cloud_transport 组件与 CLAUDE.md "P3 天气架构"一节。
 *
 * 依赖 net_manager 已联网；请求失败或返回非200 code时保留上次缓存值，只翻转
 * available标志位，不阻断UI(与claude_quota_client一致的降级语义)。
 *
 * 任务: weather_task, 栈4096, 优先级3 (登记于 docs/coding-standards.md 第6节)
 */

typedef struct {
    bool available;
    float temperature;  /* 摄氏度 */
    int humidity;         /* % */
    char condition[24];   /* 天气现象文字，如"多云"(UTF-8) */
    char wind[40];         /* 风向+风力，如"东南风3级"(UTF-8) */
    time_t fetched_at;    /* 设备本地成功拉取到数据的时间(依赖RTC/NTP已同步) */
} weather_now_t;

/* api_key: X-QW-Api-Key请求头；api_host: 项目专属域名(不含协议前缀)，如
 * "xxxxx.re.qweatherapi.com"；location: 城市LocationID，如"101010100" */
esp_err_t weather_client_init(const char *api_key, const char *api_host, const char *location);

/* 拷贝最近一次轮询结果。返回false表示启动以来从未成功获取过 */
bool weather_client_get(weather_now_t *out);

#ifdef __cplusplus
}
#endif
