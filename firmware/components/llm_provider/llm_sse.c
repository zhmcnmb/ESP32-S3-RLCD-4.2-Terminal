/* llm_sse.c - SSE 流解析 + tool_call 增量累积。
 * 从 llm_provider 抽出为独立 module，使解析逻辑可脱离 HTTP 纯测。
 * 行缓冲、JSON 解析、tool_call 累积与 finish flush 均收在此处。 */
#include "llm_sse.h"

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "llm_sse";

/* tool_call 增量累积 */
static void accumulate_tool_call(llm_sse_t *ctx, cJSON *tc_item)
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
            size_t current = strlen(slot->arguments);
            size_t fragment = strlen(args_j->valuestring);
            size_t remaining = sizeof(slot->arguments) - current - 1;
            if (fragment > remaining) {
                slot->arguments_truncated = true;
            }
            strncat(slot->arguments, args_j->valuestring, remaining);
        }
    }
}

/* finish_reason -> on_tool_calls 回调触发。模型流式工具回合 index 从 0
 * 连续递增(OpenAI 兼容规范)，无需空洞压缩，count 即实际槽位数 */
static void check_finish_and_flush(llm_sse_t *ctx, cJSON *choice)
{
    cJSON *reason = cJSON_GetObjectItem(choice, "finish_reason");
    if (!cJSON_IsString(reason) || reason->valuestring[0] == '\0') {
        return;
    }
    if (ctx->tool_call_count > 0 && ctx->sink && ctx->sink->on_tool_calls) {
        ctx->sink->on_tool_calls(ctx->tool_calls, ctx->tool_call_count, ctx->sink->ctx);
    }
}

/* 处理一条完整的 SSE 行 */
static esp_err_t process_sse_line(llm_sse_t *ctx, const char *line)
{
    if (line[0] == '\0') {
        return ESP_OK;
    }
    if (strncmp(line, "data: ", 6) != 0) {
        return ESP_OK;
    }
    const char *payload = line + 6;
    /* [DONE] 哨兵非合法JSON，与坏数据一样在 Parse 失败处静默跳过 */
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
        cJSON *tc_arr = cJSON_GetObjectItem(delta, "tool_calls");
        if (cJSON_IsArray(tc_arr)) {
            int n = cJSON_GetArraySize(tc_arr);
            for (int i = 0; i < n; i++) {
                accumulate_tool_call(ctx, cJSON_GetArrayItem(tc_arr, i));
            }
        }
        cJSON *content = cJSON_GetObjectItem(delta, "content");
        /* 已出现 tool_call 后的 content 属于工具回合，不作为回复投递 */
        if (cJSON_IsString(content) && content->valuestring[0] != '\0'
            && ctx->tool_call_count == 0
            && ctx->sink && ctx->sink->on_content_delta) {
            ctx->sink->on_content_delta(content->valuestring, ctx->sink->ctx);
            ctx->delta_delivered = true;
        }
    }
    check_finish_and_flush(ctx, choice);
    cJSON_Delete(root);
    return ESP_OK;
}

void llm_sse_init(llm_sse_t *s, const llm_stream_sink_t *sink)
{
    memset(s, 0, sizeof(*s));
    s->sink = sink;
}

/* cloud_transport 分块回调：行缓冲 + 逐行分派 */
esp_err_t llm_sse_feed(const char *data, size_t len, void *user_ctx)
{
    llm_sse_t *ctx = (llm_sse_t *)user_ctx;
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (ctx->truncated) {
                /* 行超长视为响应损坏，整个请求失败 */
                ctx->truncated = false;
                ctx->line_len = 0;
                return ESP_ERR_INVALID_RESPONSE;
            }
            ctx->line_buf[ctx->line_len] = '\0';
            esp_err_t err = process_sse_line(ctx, ctx->line_buf);
            ctx->line_len = 0;
            if (err != ESP_OK) {
                return err;
            }
        } else if (ctx->line_len < SSE_LINE_MAX - 1) {
            ctx->line_buf[ctx->line_len++] = c;
        } else {
            ctx->truncated = true;
        }
    }
    return ESP_OK;
}
