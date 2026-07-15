#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi连接管理。凭证来自 main/secrets.h，由 app_main 传入(组件不依赖上层头文件)。 */

typedef enum {
    NET_DISCONNECTED,
    NET_CONNECTING,
    NET_CONNECTED,
} net_state_t;

/* WiFi首次连接成功后触发一次的回调；回调在事件循环上下文同步执行，
 * 必须非阻塞、不做网络IO或等待锁(用于启动SNTP等一次性动作) */
typedef void (*net_connected_cb_t)(void);

/* 初始化WiFi并异步连接。不阻塞，连接结果通过 net_get_state() 查询 */
esp_err_t net_init(const char *ssid, const char *password);

net_state_t net_get_state(void);

/* 注册首次连接成功回调；必须在 net_init() 之前调用，确保不错过延迟联网场景下
 * 才到来的第一次 GOT_IP。同一时刻只支持一个订阅者(当前唯一调用方是SNTP启动) */
esp_err_t net_manager_register_connected_cb(net_connected_cb_t cb);

/* 阻塞等待连接成功，timeout_ms后超时返回ESP_ERR_TIMEOUT */
esp_err_t net_wait_connected(int timeout_ms);

#ifdef __cplusplus
}
#endif
