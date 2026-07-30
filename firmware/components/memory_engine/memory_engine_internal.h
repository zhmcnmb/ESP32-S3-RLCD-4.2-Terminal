#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "memory_engine.h"
#include "cJSON.h"

#define MEMORY_ENGINE_RECORD_CAPACITY 64
#define MEMORY_ENGINE_EVENT_PATH "agent/memory/events-0001.jsonl"
#define MEMORY_ENGINE_TOMBSTONE_PATH "agent/memory/tombstones.jsonl"
#define MEMORY_ENGINE_PROFILE_A_PATH "agent/profile/profile_a.json"
#define MEMORY_ENGINE_PROFILE_B_PATH "agent/profile/profile_b.json"
#define MEMORY_ENGINE_PROFILE_CAPACITY 6
#define MEMORY_ENGINE_PROFILE_FILE_MAX 2048
#define MEMORY_ENGINE_LINE_MAX 768

typedef struct {
    memory_record_t record;
} memory_entry_t;

typedef struct {
    memory_entry_t entries[MEMORY_ENGINE_RECORD_CAPACITY];
    size_t count;
    memory_id_t next_id;
    uint32_t profile_generation;
    bool initialized;
    bool available;
} memory_state_t;

const char *memory_type_name(memory_type_t type);
bool memory_type_parse(const char *name, memory_type_t *out_type);
uint8_t memory_type_importance(memory_type_t type);

esp_err_t memory_json_crc(const cJSON *json, uint32_t *out);
void memory_erase_entry(memory_state_t *state, size_t index);

bool memory_record_expired(const memory_record_t *record, time_t now);
esp_err_t memory_recall_build(const memory_state_t *state, const memory_query_t *query,
                              memory_context_t *out);
esp_err_t memory_store_restore(memory_state_t *state);
esp_err_t memory_store_append_event(const memory_record_t *record);
esp_err_t memory_store_append_tombstone(memory_id_t id);
esp_err_t memory_profile_restore(memory_state_t *state);
esp_err_t memory_profile_write(memory_state_t *state);

/* memory_writer：把热路径SD写收敛到单任务有界队列，避免阻塞agent任务。 */
/* 以下两个enqueue必须在持有 memory_engine s_lock 时调用（靠外层锁串行化）。 */
esp_err_t memory_writer_start(void);  /* 仅SD已挂载、恢复完成后由memory_engine_init调用 */
esp_err_t memory_writer_enqueue_line(const char *rel_path, const char *line);
esp_err_t memory_writer_enqueue_replace(const char *rel_path, const void *data, size_t len);
