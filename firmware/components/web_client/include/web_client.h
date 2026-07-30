#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Jina Search/Reader 的受限 Adapter：只暴露搜索结果和截断后的正文，隐藏 HTTP、
 * Bearer 认证与响应格式。单次响应最多接收16KB到PSRAM；正文最多返回8KB。抽取目标
 * 还必须精确匹配最近一次成功搜索返回的 URL。URL 必须是公共 DNS 主机名，拒绝
 * localhost、.local、所有字面 IP、用户信息和非 HTTP(S) scheme，避免云端 Reader
 * 成为探测内网的通道。
 */
#define WEB_CLIENT_MAX_RESULTS 5
#define WEB_CLIENT_TITLE_MAX 96
#define WEB_CLIENT_URL_MAX 256
#define WEB_CLIENT_SNIPPET_MAX 240
#define WEB_CLIENT_EXTRACT_MAX 8192

typedef struct {
    char title[WEB_CLIENT_TITLE_MAX];
    char url[WEB_CLIENT_URL_MAX];
    char snippet[WEB_CLIENT_SNIPPET_MAX];
} web_client_search_result_t;

/* api_key 为空时返回 ESP_ERR_INVALID_ARG；调用方据此不注册联网工具。 */
esp_err_t web_client_init(const char *api_key);
bool web_client_is_available(void);

/* max_results 范围1..5；返回的结果数量写入out_count。 */
esp_err_t web_client_search(const char *query, uint8_t max_results,
                            web_client_search_result_t *results, size_t result_capacity,
                            size_t *out_count);

/* max_chars 范围1..8192；out由调用方持有，返回内容总是以\0结束。 */
esp_err_t web_client_extract(const char *url, size_t max_chars, char *out,
                             size_t out_size, size_t *out_len);

/* 清空白名单，确保 extract 只在当前轮的 search 结果中选取 URL。 */
void web_client_reset_sources(void);
#ifdef __cplusplus
}
#endif

