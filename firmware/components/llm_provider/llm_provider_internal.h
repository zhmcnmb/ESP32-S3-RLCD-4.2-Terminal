#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "llm_provider.h"

/* 将统一消息和工具定义编码成 OpenAI 兼容请求体。返回的 cJSON 字符串由调用方 free。 */
esp_err_t llm_provider_build_request_body(const char *model,
                                          const llm_message_t *messages, int message_count,
                                          const llm_tool_def_t *tools, int tool_count,
                                          char **out_body, size_t *out_len);
