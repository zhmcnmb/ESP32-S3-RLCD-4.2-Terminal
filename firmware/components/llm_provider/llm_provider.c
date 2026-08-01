/*
 * LLM Provider 门面：请求体构建、SSE 流解析、tool_calls 增量累积、重试与错误分类。
 * base URL 由 app_runtime 配置注入(URL 须配置化)。
 */
#include "llm_provider.h"
#include "llm_sse.h"
#include "llm_provider_internal.h"
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cloud_transport.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "llm_provider";

/* 运行时状态 */
static bool s_initialized = false;
static char s_api_key[256];
static char s_model[128];
static char s_base_url[192]; /* 端点由 app_runtime 配置传入(URL 须配置化) */


/* 单次 HTTP 往返 */
static esp_err_t do_chat_once(const llm_message_t *messages, int message_count,
                              const llm_tool_def_t *tools, int tool_count,
                              llm_sse_t *sse_ctx,
                              const volatile bool *cancel_flag, int *out_status)
{
    char *body = NULL;
    size_t body_len = 0;
    esp_err_t err = llm_provider_build_request_body(s_model, messages, message_count,
                                                     tools, tool_count, &body, &body_len);
    if (err != ESP_OK) {
        return err;
    }
    const char *url = s_base_url;
    if (!url || !url[0]) {
        free(body);
        return ESP_ERR_INVALID_ARG;
    }
    err = cloud_transport_post_stream(url, s_api_key, "application/json",
                                      body, body_len,
                                      llm_sse_feed, sse_ctx,
                                      cancel_flag, out_status);
    free(body);
    return err;
}

/* 公开 API */
esp_err_t llm_provider_init(const char *api_key, const char *model, const char *base_url)
{
    if (!api_key || !api_key[0] || !model || !model[0] || !base_url || !base_url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(s_api_key, api_key, sizeof(s_api_key) - 1);
    s_api_key[sizeof(s_api_key) - 1] = '\0';
    strncpy(s_model, model, sizeof(s_model) - 1);
    s_model[sizeof(s_model) - 1] = '\0';
    strncpy(s_base_url, base_url, sizeof(s_base_url) - 1);
    s_base_url[sizeof(s_base_url) - 1] = '\0';
    s_initialized = true;
    ESP_LOGI(TAG, "init OK, model=%s base=%s", s_model, s_base_url);
    return ESP_OK;
}
esp_err_t llm_provider_chat(const llm_message_t *messages, int message_count,
                            const llm_tool_def_t *tools, int tool_count,
                            const llm_stream_sink_t *sink,
                            const volatile bool *cancel_flag, int *out_status)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!messages || message_count <= 0 || !sink || !out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    llm_sse_t *sse_ctx = heap_caps_calloc(1, sizeof(*sse_ctx), MALLOC_CAP_SPIRAM);
    if (!sse_ctx) {
        return ESP_ERR_NO_MEM;
    }
    *out_status = 0;
    esp_err_t result = ESP_FAIL;
    for (int attempt = 0; attempt < 2; attempt++) {
        llm_sse_init(sse_ctx, sink);
        int status = 0;
        esp_err_t err = do_chat_once(messages, message_count, tools, tool_count,
                                     sse_ctx, cancel_flag, &status);
        *out_status = status;
        if (err == ESP_OK && status >= 200 && status < 300) {
            result = ESP_OK;
            break;
        }
        result = (err != ESP_OK) ? err : ESP_ERR_HTTP_CONNECT;
        bool retryable = !sse_ctx->delta_delivered
            && ((err == ESP_ERR_HTTP_CONNECT || (err == ESP_OK && status != 0))
                && (status == 429 || status == 503 || status == 504));
        if (retryable && attempt + 1 < 2) {
            ESP_LOGW(TAG, "HTTP %d on attempt %d, retrying once...", status, attempt + 1);
            continue;
        }
        break;
    }
    free(sse_ctx);
    return result;
}
