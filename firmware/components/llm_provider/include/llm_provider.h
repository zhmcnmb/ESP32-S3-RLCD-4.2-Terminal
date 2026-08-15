#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    AGENT_ROLE_SYSTEM,
    AGENT_ROLE_USER,
    AGENT_ROLE_ASSISTANT,
    AGENT_ROLE_TOOL,
} agent_role_t;

/* 单个函数调用。arguments 是完整 JSON 对象文本，不由 provider 改写。 */
#define LLM_TOOL_CALL_ARGS_MAX 512
#define LLM_MAX_TOOL_CALLS 4
typedef struct {
    char id[64];
    char name[64];
    char arguments[LLM_TOOL_CALL_ARGS_MAX]; /* 跨 SSE chunk 累积后的 JSON */
    bool arguments_truncated; /* 超出预算时必须拒绝执行，不能使用残缺参数 */
} llm_tool_call_t;

typedef struct {
    agent_role_t role;
    const char *content;      /* system/user/assistant 文本；tool 时为工具结果 */
    const char *tool_call_id; /* 仅 AGENT_ROLE_TOOL 填写 */
    /* 仅 AGENT_ROLE_ASSISTANT 的工具请求填写。下一轮必须原样回传它们，
     * 否则 tool 结果失去与模型函数调用的关联。 */
    const llm_tool_call_t *tool_calls;
    int tool_call_count;
} llm_message_t;

typedef struct {
    const char *name;
    const char *description;
    const char *parameters_json; /* JSON Schema 字符串 */
} llm_tool_def_t;

typedef struct {
    void (*on_content_delta)(const char *delta, void *ctx); /* 纯文本回答，可能被调用多次 */
    void (*on_tool_calls)(const llm_tool_call_t *calls, int count, void *ctx); /* 工具调用最终确定后一次性回调 */
    void *ctx;
} llm_stream_sink_t;

esp_err_t llm_provider_init(const char *api_key, const char *model, const char *base_url);

esp_err_t llm_provider_chat(const llm_message_t *messages, int message_count,
                             const llm_tool_def_t *tools, int tool_count,
                             const llm_stream_sink_t *sink,
                             const volatile bool *cancel_flag, int *out_status);
