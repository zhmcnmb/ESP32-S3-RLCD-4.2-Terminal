#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 板载长期记忆的唯一 Interface。调用方只提供经过产品策略允许的候选和查询，
 * 不接触 SD 路径、日志格式、CRC、删除标记或排序实现。SD 不可用时初始化仍成功，
 * 但写入返回 ESP_ERR_INVALID_STATE，已有智能体可继续使用短会话。
 */
#define MEMORY_ENGINE_CONTENT_MAX 240
#define MEMORY_ENGINE_MAX_MATCHES 8
#define MEMORY_ENGINE_CONTEXT_MAX 2048
#define MEMORY_ENGINE_TTL_MAX_SECONDS (365U * 24U * 60U * 60U)

typedef uint32_t memory_id_t;

#define MEMORY_ID_INVALID ((memory_id_t)0)

typedef enum {
    MEMORY_TYPE_PROFILE = 0,
    MEMORY_TYPE_PREFERENCE,
    MEMORY_TYPE_FACT,
    MEMORY_TYPE_CORRECTION,
    MEMORY_TYPE_EPISODE,
} memory_type_t;

/* 只接受用户明确要求保存的简短候选；ttl_seconds为0表示不过期。 */
typedef struct {
    memory_type_t type;
    const char *content;
    uint32_t ttl_seconds;
} memory_event_t;

/* text为空时按最近更新时间列出；max_bytes限制供模型使用的上下文文本。 */
typedef struct {
    const char *text;
    uint8_t limit;
    size_t max_bytes;
    time_t now;
} memory_query_t;

typedef struct {
    memory_id_t id;
    memory_type_t type;
    char content[MEMORY_ENGINE_CONTENT_MAX + 1];
    uint8_t importance;
    time_t created_at;
    time_t expires_at; /* 0表示不过期 */
} memory_record_t;

typedef struct {
    memory_record_t records[MEMORY_ENGINE_MAX_MATCHES];
    uint8_t count;
    char context[MEMORY_ENGINE_CONTEXT_MAX];
} memory_context_t;

/* 必须在 storage_sd_init() 之后调用；重复调用安全。 */
esp_err_t memory_engine_init(void);
bool memory_engine_is_available(void);

esp_err_t memory_recall(const memory_query_t *query, memory_context_t *out);
esp_err_t memory_record(const memory_event_t *event);
esp_err_t memory_forget(memory_id_t id);

#ifdef __cplusplus
}
#endif
