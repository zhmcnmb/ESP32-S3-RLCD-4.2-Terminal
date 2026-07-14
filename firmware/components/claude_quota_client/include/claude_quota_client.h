#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Claude用量桥接客户端: 轮询Mac本地桥接服务(bridge/server.py)，展示订阅5小时/7天用量窗口。
 * 数据源已是Anthropic官方权威用量(见 docs/CLAUDE.md "P4 用量数据源")，不是估算。
 *
 * 流程: mDNS解析桥接服务主机名 -> HTTP GET /api/quota/compact -> cJSON解析 -> 缓存。
 * 依赖 net_manager 已联网；桥接服务不可达时保留上次缓存或标记 available=false，不阻断UI。
 *
 * 任务: claude_quota_task, 栈4096, 优先级3 (登记于 docs/coding-standards.md 第6节)
 */

typedef struct {
    bool available;    /* 上次轮询是否成功 */
    float used_5h;      /* 0~1 */
    float used_7d;      /* 0~1 */
    time_t resets_5h;   /* UTC epoch */
    time_t resets_7d;
    time_t fetched_at;  /* 桥接服务抓取数据的时间(UTC epoch)，展示"数据于Xs前"用 */
} claude_quota_t;

/* 启动后台轮询任务。mdns_host 不含".local"后缀(如"sunjindeMacBook-Air") */
esp_err_t claude_quota_client_init(const char *mdns_host, uint16_t port);

/* 拷贝最近一次轮询结果到out。返回false表示启动以来从未成功获取过 */
bool claude_quota_client_get(claude_quota_t *out);

#ifdef __cplusplus
}
#endif
