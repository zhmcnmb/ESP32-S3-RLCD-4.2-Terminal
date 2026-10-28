#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 直连锐奇中转公开用量 GET /v1/usage（dashboard 同源只读接口）。
 * api_key 为 sk-，不使用管理账号、浏览器 Cookie。
 *
 * 任务: codex_usage_task, 栈8192, 优先级3。
 */
esp_err_t codex_usage_client_init(const char *base_url, const char *api_key);

#ifdef __cplusplus
}
#endif
