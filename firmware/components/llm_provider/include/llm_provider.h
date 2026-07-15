#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    LLM_PROVIDER_SILICONFLOW,
    LLM_PROVIDER_DEEPSEEK,
} llm_provider_kind_t;

typedef enum {
    AGENT_ROLE_SYSTEM,
    AGENT_ROLE_USER,
    AGENT_ROLE_ASSISTANT,
    AGENT_ROLE_TOOL,
} agent_role_t;

typedef struct {
    agent_role_t role;
    const char *content;      /* system/user/assistant文本；AGENT_ROLE_TOOL时是工具执行结果文本 */
    const char *tool_call_id; /* 仅AGENT_ROLE_TOOL填写：对应助手发起的tool_call id，其余角色传NULL */
} llm_message_t;

typedef struct {
    const char *name;
    const char *description;
    const char *parameters_json; /* JSON Schema字符串，如 "{\"type\":\"object\",\"properties\":{}}" */
} llm_tool_def_t;

#define LLM_TOOL_CALL_ARGS_MAX 512
#define LLM_MAX_TOOL_CALLS 4

typedef struct {
    char id[64];
    char name[64];
    char arguments[LLM_TOOL_CALL_ARGS_MAX]; /* 跨SSE chunk累积后的完整JSON字符串 */
} llm_tool_call_t;

typedef struct {
    void (*on_content_delta)(const char *delta, void *ctx); /* 纯文本回答，可能被调用多次 */
    void (*on_tool_calls)(const llm_tool_call_t *calls, int count, void *ctx); /* 工具调用最终确定后一次性回调 */
    void *ctx;
} llm_stream_sink_t;

esp_err_t llm_provider_init(llm_provider_kind_t provider, const char *api_key, const char *model);

esp_err_t llm_provider_chat(const llm_message_t *messages, int message_count,
                             const llm_tool_def_t *tools, int tool_count,
                             const llm_stream_sink_t *sink,
                             const volatile bool *cancel_flag, int *out_status);
