#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 直连上游用量服务的公开只读 GET /v1/usage（与仪表盘同源）。
 * api_key 为 sk-，不使用管理账号、浏览器 Cookie。
 *
 * 任务: codex_usage_task, 栈8192, 优先级3。
 */
esp_err_t codex_usage_client_init(const char *base_url, const char *api_key);

#ifdef __cplusplus
}
#endif
