#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "cJSON.h"
#include "llm_provider.h"

#define AGENT_MAX_ROUNDTRIPS 4
#define AGENT_CONTEXT_HISTORY_MAX 10
#define AGENT_CONTEXT_TEXT_MAX 768
#define AGENT_TOOL_RESULT_MAX (18 * 1024)
#define AGENT_TURN_TOOL_SLOTS LLM_MAX_TOOL_CALLS
#define AGENT_MESSAGE_MAX (1 + AGENT_CONTEXT_HISTORY_MAX + 1 + AGENT_MAX_ROUNDTRIPS \
                           * (1 + LLM_MAX_TOOL_CALLS))

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool oom;
} agent_text_t;

void agent_text_init(agent_text_t *text);
void agent_text_reset(agent_text_t *text);
esp_err_t agent_text_append(agent_text_t *text, const char *data, size_t len);
const char *agent_text_c_str(const agent_text_t *text);
size_t agent_text_len(const agent_text_t *text);
bool agent_text_oom(const agent_text_t *text);

void agent_session_init(void);
int agent_session_copy_context(llm_message_t *out, int capacity);
void agent_session_record_message(agent_role_t role, const char *content,
                                  const char *tool_call_id, bool include_context);
void agent_session_record_tool_calls(const llm_tool_call_t *calls, int count);

const llm_tool_def_t *agent_tools_get_definitions(int *out_count);
esp_err_t agent_tools_execute(const llm_tool_call_t *call, char *out, size_t out_len);
const char *const *agent_tools_get_skill_allowed_tools(size_t *out_count);
void agent_memory_tools_prepare(const char *user_text);
const llm_tool_def_t *agent_memory_tools_get_definitions(int *out_count);
bool agent_memory_tools_handles(const char *name);
esp_err_t agent_memory_tools_execute(const char *name, const cJSON *args, cJSON **out);
void agent_scheduler_tools_prepare(const char *user_text);
const llm_tool_def_t *agent_scheduler_tools_get_definitions(int *out_count);
bool agent_scheduler_tools_handles(const char *name);
esp_err_t agent_scheduler_tools_execute(const char *name, const cJSON *args, cJSON **out);

/* agent_args.c：工具参数校验共享 helper，各工具 adapter 复用 */
/* 校验 args 恰好含 required 全部字段与 optional 任意子集，无其他字段；
 * required_count==0 且 optional_count==0 时要求 args 为空对象。
 * 字段名重复只要求出现一次（如 "limit","limit" 即单个必需字段）。 */
bool agent_args_validate(const cJSON *args, const char *const required[], int required_count,
                         const char *const optional[], int optional_count);
/* 解析 uint 参数：整数且 min<=v<=max，否则 false */
bool agent_args_parse_uint(const cJSON *args, const char *name, uint32_t min,
                           uint32_t max, uint32_t *out);
