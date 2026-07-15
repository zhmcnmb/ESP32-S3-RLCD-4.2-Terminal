/*
 * LLM Provider 门面：请求体构建、SSE 流解析、tool_calls 增量累积、重试与错误分类。
 * base URL 由各 provider 专用 .c 提供，本文件按 kind 分发。
 */
#include "llm_provider.h"
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cloud_transport.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "llm_provider";

/* 运行时状态 */
static bool s_initialized = false;
static llm_provider_kind_t s_provider_kind;
static char s_api_key[256];
static char s_model[128];

#define SSE_LINE_MAX 4096

/* 单次 chat 往返中跨 chunk callback 维护的 SSE 解析 + tool call 累积 */
typedef struct {
    char line_buf[SSE_LINE_MAX];
    size_t line_len;
    llm_tool_call_t tool_calls[LLM_MAX_TOOL_CALLS];
    int tool_call_count;          /* 已见过的最大 index + 1 */
    const llm_stream_sink_t *sink;
} sse_ctx_t;

/* provider URL 分发 */
extern const char *llm_provider_get_siliconflow_url(void);
extern const char *llm_provider_get_deepseek_url(void);

static const char *get_base_url(llm_provider_kind_t kind)
{
    switch (kind) {
    case LLM_PROVIDER_SILICONFLOW:
        return llm_provider_get_siliconflow_url();
    case LLM_PROVIDER_DEEPSEEK:
        return llm_provider_get_deepseek_url();
    default:
        return NULL;
    }
}

/* 请求体构建（辅助） */
static esp_err_t add_messages_to_array(cJSON *msgs,
                                       const llm_message_t *messages, int message_count)
{
    for (int i = 0; i < message_count; i++) {
        cJSON *m = cJSON_CreateObject();
        if (!m) {
            return ESP_ERR_NO_MEM;
        }
        switch (messages[i].role) {
        case AGENT_ROLE_SYSTEM:
            cJSON_AddStringToObject(m, "role", "system"); break;
        case AGENT_ROLE_USER:
            cJSON_AddStringToObject(m, "role", "user"); break;
        case AGENT_ROLE_ASSISTANT:
            cJSON_AddStringToObject(m, "role", "assistant"); break;
        case AGENT_ROLE_TOOL:
            cJSON_AddStringToObject(m, "role", "tool");
            if (messages[i].tool_call_id) {
                cJSON_AddStringToObject(m, "tool_call_id", messages[i].tool_call_id);
            }
            break;
        }
        if (messages[i].content) {
            cJSON_AddStringToObject(m, "content", messages[i].content);
        } else {
            cJSON_AddNullToObject(m, "content");
        }
        cJSON_AddItemToArray(msgs, m);
    }
    return ESP_OK;
}

static esp_err_t add_tools_to_array(cJSON *arr,
                                    const llm_tool_def_t *tools, int tool_count)
{
    for (int i = 0; i < tool_count; i++) {
        cJSON *t = cJSON_CreateObject();
        if (!t) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(t, "type", "function");
        cJSON *fn = cJSON_AddObjectToObject(t, "function");
        cJSON_AddStringToObject(fn, "name", tools[i].name);
        cJSON_AddStringToObject(fn, "description", tools[i].description);
        if (tools[i].parameters_json) {
            cJSON *params = cJSON_Parse(tools[i].parameters_json);
            if (params) {
                cJSON_AddItemToObject(fn, "parameters", params);
            }
        }
        cJSON_AddItemToArray(arr, t);
    }
    return ESP_OK;
}

static esp_err_t build_request_body(const char *model,
                                    const llm_message_t *messages, int message_count,
                                    const llm_tool_def_t *tools, int tool_count,
                                    char **out_body, size_t *out_len)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddBoolToObject(root, "stream", true);
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    esp_err_t err = add_messages_to_array(msgs, messages, message_count);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return err;
    }
    if (tools && tool_count > 0) {
        cJSON *arr = cJSON_AddArrayToObject(root, "tools");
        err = add_tools_to_array(arr, tools, tool_count);
        if (err != ESP_OK) {
            cJSON_Delete(root);
            return err;
        }
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    *out_body = json;
    *out_len  = strlen(json);
    return ESP_OK;
}

/* tool_call 增量累积 */
static void accumulate_tool_call(sse_ctx_t *ctx, cJSON *tc_item)
{
    cJSON *idx_j = cJSON_GetObjectItem(tc_item, "index");
    if (!cJSON_IsNumber(idx_j)) {
        return;
    }
    int idx = idx_j->valueint;
    if (idx < 0 || idx >= LLM_MAX_TOOL_CALLS) {
        ESP_LOGW(TAG, "tool_call index %d out of range [0, %d)", idx, LLM_MAX_TOOL_CALLS);
        return;
    }
    llm_tool_call_t *slot = &ctx->tool_calls[idx];
    /* 判断是否新 tool_call 首chunk：出现 id 或 function.name */
    cJSON *id_j = cJSON_GetObjectItem(tc_item, "id");
    cJSON *fn   = cJSON_GetObjectItem(tc_item, "function");
    bool is_new = (id_j && cJSON_IsString(id_j) && id_j->valuestring[0] != '\0');
    if (!is_new && fn) {
        cJSON *name_j = cJSON_GetObjectItem(fn, "name");
        is_new = (name_j && cJSON_IsString(name_j) && name_j->valuestring[0] != '\0');
    }
    if (is_new) {
        memset(slot, 0, sizeof(*slot));
        if (id_j && cJSON_IsString(id_j)) {
            strncpy(slot->id, id_j->valuestring, sizeof(slot->id) - 1);
        }
        if (fn) {
            cJSON *name_j = cJSON_GetObjectItem(fn, "name");
            if (name_j && cJSON_IsString(name_j)) {
                strncpy(slot->name, name_j->valuestring, sizeof(slot->name) - 1);
            }
        }
        if (idx + 1 > ctx->tool_call_count) {
            ctx->tool_call_count = idx + 1;
        }
    }
    if (fn) {
        cJSON *args_j = cJSON_GetObjectItem(fn, "arguments");
        if (args_j && cJSON_IsString(args_j) && args_j->valuestring[0] != '\0') {
            size_t cur = strlen(slot->arguments);
            size_t rem = sizeof(slot->arguments) - cur - 1;
            strncat(slot->arguments, args_j->valuestring, rem);
        }
    }
}

/* finish_reason → on_tool_calls 回调触发 */
static void check_finish_and_flush(sse_ctx_t *ctx, cJSON *choice)
{
    cJSON *reason = cJSON_GetObjectItem(choice, "finish_reason");
    if (!cJSON_IsString(reason) || reason->valuestring[0] == '\0') {
        return;
    }
    bool has_tool_calls = false;
    for (int i = 0; i < ctx->tool_call_count && !has_tool_calls; i++) {
        if (ctx->tool_calls[i].name[0] != '\0') {
            has_tool_calls = true;
        }
    }
    if (has_tool_calls && ctx->sink && ctx->sink->on_tool_calls) {
        ctx->sink->on_tool_calls(ctx->tool_calls, ctx->tool_call_count, ctx->sink->ctx);
    }
}

/* 处理一条完整的 SSE 行 */
static esp_err_t process_sse_line(sse_ctx_t *ctx, const char *line)
{
    if (line[0] == '\0') {
        return ESP_OK;
    }
    if (strncmp(line, "data: ", 6) != 0) {
        return ESP_OK;
    }
    const char *payload = line + 6;
    if (strcmp(payload, "[DONE]") == 0) {
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        return ESP_OK;
    }
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        cJSON_Delete(root);
        return ESP_OK;
    }
    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    cJSON *delta  = cJSON_GetObjectItem(choice, "delta");
    if (delta) {
        cJSON *content = cJSON_GetObjectItem(delta, "content");
        if (cJSON_IsString(content) && content->valuestring[0] != '\0'
            && ctx->sink && ctx->sink->on_content_delta) {
            ctx->sink->on_content_delta(content->valuestring, ctx->sink->ctx);
        }
    }
    if (delta) {
        cJSON *tc_arr = cJSON_GetObjectItem(delta, "tool_calls");
        if (cJSON_IsArray(tc_arr)) {
            int n = cJSON_GetArraySize(tc_arr);
            for (int i = 0; i < n; i++) {
                accumulate_tool_call(ctx, cJSON_GetArrayItem(tc_arr, i));
            }
        }
    }
    check_finish_and_flush(ctx, choice);
    cJSON_Delete(root);
    return ESP_OK;
}

/* cloud_transport 分块回调：行缓冲 + 逐行分派 */
static esp_err_t sse_chunk_cb(const char *data, size_t len, void *user_ctx)
{
    sse_ctx_t *ctx = (sse_ctx_t *)user_ctx;
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            ctx->line_buf[ctx->line_len] = '\0';
            esp_err_t err = process_sse_line(ctx, ctx->line_buf);
            ctx->line_len = 0;
            if (err != ESP_OK) {
                return err;
            }
        } else if (ctx->line_len < SSE_LINE_MAX - 1) {
            ctx->line_buf[ctx->line_len++] = c;
        }
    }
    return ESP_OK;
}

/* 单次 HTTP 往返 */
static esp_err_t do_chat_once(const llm_message_t *messages, int message_count,
                              const llm_tool_def_t *tools, int tool_count,
                              sse_ctx_t *sse_ctx,
                              const volatile bool *cancel_flag, int *out_status)
{
    char *body = NULL;
    size_t body_len = 0;
    esp_err_t err = build_request_body(s_model, messages, message_count,
                                       tools, tool_count, &body, &body_len);
    if (err != ESP_OK) {
        return err;
    }
    const char *url = get_base_url(s_provider_kind);
    if (!url) {
        free(body);
        return ESP_ERR_INVALID_ARG;
    }
    err = cloud_transport_post_stream(url, s_api_key, "application/json",
                                      body, body_len,
                                      sse_chunk_cb, sse_ctx,
                                      cancel_flag, out_status);
    free(body);
    return err;
}

/* 公开 API */
esp_err_t llm_provider_init(llm_provider_kind_t provider, const char *api_key, const char *model)
{
    if (!api_key || !api_key[0] || !model || !model[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    s_provider_kind = provider;
    strncpy(s_api_key, api_key, sizeof(s_api_key) - 1);
    s_api_key[sizeof(s_api_key) - 1] = '\0';
    strncpy(s_model, model, sizeof(s_model) - 1);
    s_model[sizeof(s_model) - 1] = '\0';
    s_initialized = true;
    ESP_LOGI(TAG, "init OK, provider=%s model=%s",
             provider == LLM_PROVIDER_SILICONFLOW ? "siliconflow" : "deepseek",
             s_model);
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
    *out_status = 0;
    sse_ctx_t sse_ctx;
    /* 最多一次自动重试 */
    for (int attempt = 0; attempt < 2; attempt++) {
        memset(&sse_ctx, 0, sizeof(sse_ctx));
        sse_ctx.sink = sink;
        int status = 0;
        esp_err_t err = do_chat_once(messages, message_count,
                                     tools, tool_count,
                                     &sse_ctx, cancel_flag, &status);
        *out_status = status;
        if (err == ESP_OK && status >= 200 && status < 300) {
            return ESP_OK;
        }
        bool retryable = ((err == ESP_ERR_HTTP_CONNECT || (err == ESP_OK && status != 0))
                         && (status == 429 || status == 503 || status == 504));
        if (retryable && attempt + 1 < 2) {
            ESP_LOGW(TAG, "HTTP %d on attempt %d, retrying once...", status, attempt + 1);
            continue;
        }
        return (err != ESP_OK) ? err : ESP_ERR_HTTP_CONNECT;
    }
    return ESP_FAIL;
}
