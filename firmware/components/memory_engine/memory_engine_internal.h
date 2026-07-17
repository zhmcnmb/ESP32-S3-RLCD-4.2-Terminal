#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "memory_engine.h"

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
    bool deleted;
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


bool memory_record_expired(const memory_record_t *record, time_t now);
esp_err_t memory_recall_build(const memory_state_t *state, const memory_query_t *query,
                              memory_context_t *out);
esp_err_t memory_store_restore(memory_state_t *state);
esp_err_t memory_store_append_event(const memory_record_t *record);
esp_err_t memory_store_append_tombstone(memory_id_t id);
esp_err_t memory_profile_restore(memory_state_t *state);
esp_err_t memory_profile_write(memory_state_t *state);
