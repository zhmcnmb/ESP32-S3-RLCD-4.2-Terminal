#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 天气客户端: 轮询Mac本地桥接服务(bridge/server.py)的 /api/weather，展示当前实况。
 *
 * 为什么不直连QWeather: QWeather新版API Host强制gzip压缩响应，esp_http_client不
 * 做自动解压，ESP32上手写gzip解压+引入mbedtls证书链工程量不小。改为让桥接服务
 * (Python标准库一行gzip.decompress()) 代理转发，ESP32复用Claude用量已验证过的
 * mDNS发现机制走局域网明文HTTP，不需要HTTPS/TLS。详见 bridge/server.py 头注释。
 *
 * 依赖 net_manager 已联网；桥接服务不可达时保留上次缓存或标记 available=false，不阻断UI。
 *
 * 任务: weather_task, 栈4096, 优先级3 (登记于 docs/coding-standards.md 第6节)
 */

typedef struct {
    bool available;
    float temperature;  /* 摄氏度 */
    int humidity;         /* % */
    char condition[24];   /* 天气现象文字，如"多云"(UTF-8) */
    char wind[40];         /* 风向+风力，如"东南风3级"(UTF-8) */
    time_t fetched_at;    /* 桥接服务抓取数据的时间(UTC epoch) */
} weather_now_t;

/* mdns_host 不含".local"后缀，同 claude_quota_client 复用同一个桥接服务地址 */
esp_err_t weather_client_init(const char *mdns_host, uint16_t port);

/* 拷贝最近一次轮询结果。返回false表示启动以来从未成功获取过 */
bool weather_client_get(weather_now_t *out);

#ifdef __cplusplus
}
#endif
