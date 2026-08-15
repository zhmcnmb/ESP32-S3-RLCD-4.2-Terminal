#pragma once

#include "llm_provider.h"

#define SSE_LINE_MAX 4096

/* 单次 chat 往返中跨 chunk callback 维护的 SSE 解析 + tool call 累积。
 * 从 llm_provider 抽出，使其可脱离 HTTP 独立纯测。 */
typedef struct {
    char line_buf[SSE_LINE_MAX];
    size_t line_len;
    llm_tool_call_t tool_calls[LLM_MAX_TOOL_CALLS];
    int tool_call_count;          /* 已见过的最大 index + 1 */
    const llm_stream_sink_t *sink;
    bool delta_delivered;         /* 本轮已投递任何 content delta */
    bool truncated;               /* 当前行被截断 */
} llm_sse_t;

/* 绑定 sink 并清零状态；每轮 chat 的每次 attempt 前调用 */
void llm_sse_init(llm_sse_t *s, const llm_stream_sink_t *sink);

/* cloud_transport 分块回调：行缓冲 + 逐行分派，超长行整请求失败 */
esp_err_t llm_sse_feed(const char *data, size_t len, void *ctx);
