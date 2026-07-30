/* memory_profile.c - 用户画像的双槽快照，事件日志始终是事实来源。 */
#include "memory_engine_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "storage_sd.h"

static const char *TAG = "memory_profile";

typedef struct {
    memory_record_t records[MEMORY_ENGINE_PROFILE_CAPACITY];
    uint8_t count;
    uint32_t generation;
    bool valid;
} profile_slot_t;


static bool parse_u32(const cJSON *item, uint32_t *out)
{
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > UINT32_MAX
        || item->valuedouble != (double)(uint32_t)item->valuedouble) return false;
    *out = (uint32_t)item->valuedouble;
    return true;
}

static bool parse_time(const cJSON *item, time_t *out)
{
    if (cJSON_IsNull(item)) {
        *out = 0;
        return true;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > INT32_MAX
        || item->valuedouble != (double)(time_t)item->valuedouble) return false;
    *out = (time_t)item->valuedouble;
    return true;
}

static bool valid_crc(cJSON *json)
{
    cJSON *stored = cJSON_DetachItemFromObjectCaseSensitive(json, "crc32");
    uint32_t stored_crc = 0;
    uint32_t computed = 0;
    bool valid = false;
    if (parse_u32(stored, &stored_crc) && memory_json_crc(json, &computed) == ESP_OK) {
        valid = (stored_crc == computed);
    }
    cJSON_Delete(stored);
    return valid;
}

static bool parse_record(const cJSON *json, memory_record_t *out)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "id");
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(json, "content");
    const cJSON *created = cJSON_GetObjectItemCaseSensitive(json, "created_at");
    const cJSON *expires = cJSON_GetObjectItemCaseSensitive(json, "expires_at");
    uint32_t parsed_id = 0;
    if (!cJSON_IsObject(json) || !parse_u32(id, &parsed_id) || parsed_id == MEMORY_ID_INVALID
        || !cJSON_IsString(content) || !content->valuestring[0]
        || strlen(content->valuestring) > MEMORY_ENGINE_CONTENT_MAX || !cJSON_IsNumber(created)
        || !parse_time(created, &out->created_at) || !parse_time(expires, &out->expires_at)) {
        return false;
    }
    out->id = parsed_id;
    out->type = MEMORY_TYPE_PROFILE;
    out->importance = memory_type_importance(MEMORY_TYPE_PROFILE);
    strcpy(out->content, content->valuestring);
    return true;
}

static bool parse_slot(const char *text, profile_slot_t *out)
{
    cJSON *json = cJSON_Parse(text);
    const cJSON *version = json ? cJSON_GetObjectItemCaseSensitive(json, "v") : NULL;
    const cJSON *generation = json ? cJSON_GetObjectItemCaseSensitive(json, "generation") : NULL;
    const cJSON *records = json ? cJSON_GetObjectItemCaseSensitive(json, "records") : NULL;
    uint32_t parsed_generation = 0;
    bool valid = json && valid_crc(json) && cJSON_IsNumber(version) && version->valueint == 1
                 && parse_u32(generation, &parsed_generation) && parsed_generation != 0
                 && cJSON_IsArray(records) && cJSON_GetArraySize(records) <= MEMORY_ENGINE_PROFILE_CAPACITY;
    for (int i = 0; valid && i < cJSON_GetArraySize(records); i++) {
        memory_record_t *record = &out->records[out->count];
        if (!parse_record(cJSON_GetArrayItem(records, i), record)) {
            valid = false;
            continue;
        }
        for (uint8_t j = 0; j < out->count; j++) {
            if (out->records[j].id == record->id) valid = false;
        }
        if (valid) out->count++;
    }
    if (valid) {
        out->generation = parsed_generation;
        out->valid = true;
    }
    cJSON_Delete(json);
    return valid;
}

static void read_slot(const char *path, profile_slot_t *out)
{
    uint32_t size = 0;
    esp_err_t err = storage_sd_get_file_size(path, &size);
    if (err == ESP_ERR_NOT_FOUND) return;
    if (err != ESP_OK || size == 0 || size > MEMORY_ENGINE_PROFILE_FILE_MAX) {
        ESP_LOGW(TAG, "画像槽不可读: %s", path);
        return;
    }
    char *text = heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM);
    if (!text) return;
    int read = storage_sd_read_chunk(path, 0, text, size);
    if (read == (int)size) {
        text[size] = '\0';
        (void)parse_slot(text, out);
    }
    free(text);
}

static void merge_slot(memory_state_t *state, const profile_slot_t *slot)
{
    for (uint8_t i = 0; i < slot->count && state->count < MEMORY_ENGINE_RECORD_CAPACITY; i++) {
        const memory_record_t *record = &slot->records[i];
        bool exists = false;
        for (size_t j = 0; j < state->count; j++) {
            if (state->entries[j].record.id == record->id) exists = true;
        }
        if (exists) continue;
        state->entries[state->count++].record = *record;
        if (record->id == UINT32_MAX) state->next_id = MEMORY_ID_INVALID;
        else if (state->next_id != MEMORY_ID_INVALID && record->id >= state->next_id) {
            state->next_id = record->id + 1;
        }
    }
}

esp_err_t memory_profile_restore(memory_state_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;
    /* 双槽约3KB；恢复由main_task发起，放PSRAM避免挤占启动栈。 */
    profile_slot_t *slots = heap_caps_calloc(2, sizeof(*slots), MALLOC_CAP_SPIRAM);
    if (!slots) return ESP_ERR_NO_MEM;
    read_slot(MEMORY_ENGINE_PROFILE_A_PATH, &slots[0]);
    read_slot(MEMORY_ENGINE_PROFILE_B_PATH, &slots[1]);
    const profile_slot_t *latest = slots[0].valid
                                 && (!slots[1].valid || slots[0].generation >= slots[1].generation)
                                 ? &slots[0] : slots[1].valid ? &slots[1] : NULL;
    if (latest) {
        merge_slot(state, latest);
        state->profile_generation = latest->generation;
    }
    free(slots);
    return ESP_OK;
}

static esp_err_t add_snapshot_record(cJSON *records, const memory_record_t *record)
{
    cJSON *json = cJSON_CreateObject();
    if (!json || !cJSON_AddNumberToObject(json, "id", record->id)
        || !cJSON_AddStringToObject(json, "content", record->content)
        || !cJSON_AddNumberToObject(json, "created_at", (double)record->created_at)) {
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    bool expiry_added = record->expires_at
                        ? cJSON_AddNumberToObject(json, "expires_at", (double)record->expires_at)
                        : cJSON_AddNullToObject(json, "expires_at");
    if (!expiry_added || !cJSON_AddItemToArray(records, json)) {
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t memory_profile_write(memory_state_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;
    uint32_t generation = state->profile_generation == UINT32_MAX ? 1 : state->profile_generation + 1;
    cJSON *json = cJSON_CreateObject();
    cJSON *records = json ? cJSON_AddArrayToObject(json, "records") : NULL;
    if (!records || !cJSON_AddNumberToObject(json, "v", 1)
        || !cJSON_AddNumberToObject(json, "generation", generation)) {
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    time_t now = time(NULL);
    for (size_t i = 0; i < state->count; i++) {
        const memory_record_t *record = &state->entries[i].record;
        if (record->type != MEMORY_TYPE_PROFILE || memory_record_expired(record, now)) continue;
        esp_err_t err = add_snapshot_record(records, record);
        if (err != ESP_OK) {
            cJSON_Delete(json);
            return err;
        }
    }
    uint32_t crc = 0;
    esp_err_t crc_err = memory_json_crc(json, &crc);
    if (crc_err != ESP_OK || !cJSON_AddNumberToObject(json, "crc32", crc)) {
        cJSON_Delete(json);
        return crc_err != ESP_OK ? crc_err : ESP_ERR_NO_MEM;
    }
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return ESP_ERR_NO_MEM;
    size_t size = strlen(text);
    if (size > MEMORY_ENGINE_PROFILE_FILE_MAX) {
        free(text);
        return ESP_ERR_INVALID_SIZE;
    }
    const char *path = generation & 1 ? MEMORY_ENGINE_PROFILE_A_PATH : MEMORY_ENGINE_PROFILE_B_PATH;
    esp_err_t err = memory_writer_enqueue_replace(path, text, size);
    free(text);
    if (err == ESP_OK) state->profile_generation = generation;
    return err;
}
