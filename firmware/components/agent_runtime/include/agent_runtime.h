#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 设备端个人智能体：维护当前会话，按消息 -> 模型 -> 工具串行循环生成最终回答。
 * 不知道 UI、音频、供应商 URL 或 SD 文件布局；这些细节分别留在调用方、
 * llm_provider 和 storage_sd/内部 session 实现。
 *
 * init 可在无 SD 卡时成功，届时退化为 RAM 短会话；必须在 storage_sd_init()
 * 之后调用，已挂载时会恢复 current.jsonl 的最近上下文。
 */
esp_err_t agent_runtime_init(void);

/* 工具运行状态由调用方映射到自己的表现层；active=false 表示本轮工具已结束。 */
typedef void (*agent_runtime_tool_state_cb_t)(bool active, void *ctx);

/* 最终回答的流式文本片段。若模型转入工具调用，本回合不得先输出面向用户的文本。 */
typedef void (*agent_runtime_reply_delta_cb_t)(const char *delta, void *ctx);

typedef struct {
    const volatile bool *cancel_flag;
    agent_runtime_tool_state_cb_t tool_state_cb;
    void *tool_state_ctx;
    agent_runtime_reply_delta_cb_t reply_delta_cb;
    void *reply_delta_ctx;
} agent_runtime_turn_options_t;

/*
 * 执行一轮：保存 user 消息，最多进行 4 次模型往返和 4 次串行只读工具调用。
 * 成功时 out_reply 指向模块持有的完整 UTF-8 文本，直到下一次 run_turn() 前有效；
 * reply_delta_cb 只接收模型最终回答的流式片段，调用方不得在回调中阻塞。
 */
esp_err_t agent_runtime_run_turn(const char *user_text,
                                 const agent_runtime_turn_options_t *options,
                                 const char **out_reply);

#ifdef __cplusplus
}
#endif
