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

/* 初始化WiFi并异步连接。不阻塞，连接结果通过 net_get_state() 查询 */
esp_err_t net_init(const char *ssid, const char *password);

net_state_t net_get_state(void);

/* 阻塞等待连接成功，timeout_ms后超时返回ESP_ERR_TIMEOUT */
esp_err_t net_wait_connected(int timeout_ms);

#ifdef __cplusplus
}
#endif
