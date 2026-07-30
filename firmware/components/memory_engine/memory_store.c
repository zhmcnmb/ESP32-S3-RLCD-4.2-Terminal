/* memory_store.c - 记忆JSONL日志的校验、追加与恢复。 */
#include "memory_engine_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "storage_sd.h"

static const char *TAG = "memory_store";

typedef esp_err_t (*memory_line_cb_t)(const char *line, void *ctx);

esp_err_t memory_json_crc(const cJSON *json, uint32_t *out)
{
    if (!json || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    char *text = cJSON_PrintUnformatted(json);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    *out = esp_crc32_le(0, (const uint8_t *)text, (uint32_t)strlen(text));
    free(text);
    return ESP_OK;
}

static esp_err_t append_json(const char *path, cJSON *json)
{
    uint32_t crc = 0;
    esp_err_t err = memory_json_crc(json, &crc);
    if (err != ESP_OK || !cJSON_AddNumberToObject(json, "crc32", crc)) {
        cJSON_Delete(json);
        return err != ESP_OK ? err : ESP_ERR_NO_MEM;
    }
    char *line = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!line) return ESP_ERR_NO_MEM;
    err = memory_writer_enqueue_line(path, line);
    free(line);
    return err;
}

static bool detach_valid_crc(cJSON *json)
{
    cJSON *stored = cJSON_DetachItemFromObjectCaseSensitive(json, "crc32");
    if (!cJSON_IsNumber(stored) || stored->valuedouble < 0 || stored->valuedouble > UINT32_MAX
        || stored->valuedouble != (double)(uint32_t)stored->valuedouble) {
        cJSON_Delete(stored);
        return false;
    }
    uint32_t computed = 0;
    if (memory_json_crc(json, &computed) != ESP_OK) {
        cJSON_Delete(stored);
        return false;
    }
    bool valid = (uint32_t)stored->valuedouble == computed;
    cJSON_Delete(stored);
    return valid;
}

static bool parse_time_field(const cJSON *json, const char *name, time_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    if (cJSON_IsNull(item)) {
        *out = 0;
        return true;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > INT32_MAX
        || item->valuedouble != (double)(time_t)item->valuedouble) return false;
    *out = (time_t)item->valuedouble;
    return true;
}

static bool parse_event(cJSON *json, memory_record_t *out)
{
    if (!detach_valid_crc(json)) return false;
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(json, "v");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "id");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(json, "content");
    const cJSON *importance = cJSON_GetObjectItemCaseSensitive(json, "importance");
    if (!cJSON_IsNumber(version) || version->valueint != 1 || !cJSON_IsNumber(id)
        || id->valuedouble < 1 || id->valuedouble > UINT32_MAX
        || id->valuedouble != (double)(uint32_t)id->valuedouble || !cJSON_IsString(type)
        || !memory_type_parse(type->valuestring, &out->type) || !cJSON_IsString(content)
        || !content->valuestring[0] || strlen(content->valuestring) > MEMORY_ENGINE_CONTENT_MAX
        || !cJSON_IsNumber(importance) || importance->valueint < 1 || importance->valueint > 100
        || !parse_time_field(json, "created_at", &out->created_at)
        || !parse_time_field(json, "expires_at", &out->expires_at)) return false;
    out->id = (memory_id_t)id->valuedouble;
    out->importance = (uint8_t)importance->valueint;
    strcpy(out->content, content->valuestring);
    return true;
}

void memory_erase_entry(memory_state_t *state, size_t index)
{
    if (!state || index >= state->count) {
        return;
    }
    if (index + 1 < state->count) {
        memmove(&state->entries[index], &state->entries[index + 1],
                (state->count - index - 1) * sizeof(state->entries[0]));
    }
    state->count--;
}

static esp_err_t restore_event_line(const char *line, void *ctx)
{
    memory_state_t *state = ctx;
    cJSON *json = cJSON_Parse(line);
    memory_record_t record = {0};
    bool valid = json && parse_event(json, &record);
    cJSON_Delete(json);
    if (!valid) return ESP_OK; /* 撕裂末行、旧版本或CRC错误都不进入可见记忆。 */
    for (size_t i = 0; i < state->count; i++) {
        if (state->entries[i].record.id == record.id) return ESP_OK;
    }
    if (state->count >= MEMORY_ENGINE_RECORD_CAPACITY) return ESP_ERR_INVALID_SIZE;
    state->entries[state->count++].record = record;
    if (record.id == UINT32_MAX) state->next_id = MEMORY_ID_INVALID;
    else if (state->next_id != MEMORY_ID_INVALID && record.id >= state->next_id) {
        state->next_id = record.id + 1;
    }
    return ESP_OK;
}

static esp_err_t restore_tombstone_line(const char *line, void *ctx)
{
    memory_state_t *state = ctx;
    cJSON *json = cJSON_Parse(line);
    bool valid = json && detach_valid_crc(json);
    const cJSON *id = valid ? cJSON_GetObjectItemCaseSensitive(json, "id") : NULL;
    if (valid && cJSON_IsNumber(id) && id->valuedouble >= 1 && id->valuedouble <= UINT32_MAX
        && id->valuedouble == (double)(uint32_t)id->valuedouble) {
        memory_id_t target = (memory_id_t)id->valuedouble;
        for (size_t i = 0; i < state->count; i++) {
            if (state->entries[i].record.id == target) {
                memory_erase_entry(state, i);
                break;
            }
        }
    }
    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t read_lines(const char *path, memory_line_cb_t callback, void *ctx)
{
    uint32_t size = 0;
    esp_err_t size_err = storage_sd_get_file_size(path, &size);
    if (size_err == ESP_ERR_NOT_FOUND) return ESP_OK;
    if (size_err != ESP_OK) return size_err;
    char line[MEMORY_ENGINE_LINE_MAX + 1];
    uint8_t chunk[256];
    size_t length = 0;
    bool discard = false;
    for (uint32_t offset = 0; offset < size;) {
        int read = storage_sd_read_chunk(path, offset, chunk, sizeof(chunk));
        if (read <= 0) return ESP_FAIL;
        offset += (uint32_t)read;
        for (int i = 0; i < read; i++) {
            if (chunk[i] == '\n') {
                if (!discard && length > 0) {
                    line[length] = '\0';
                    esp_err_t err = callback(line, ctx);
                    if (err != ESP_OK) return err;
                }
                length = 0;
                discard = false;
            } else if (length < MEMORY_ENGINE_LINE_MAX) {
                line[length++] = (char)chunk[i];
            } else {
                discard = true;
            }
        }
    }
    return ESP_OK; /* 没有换行的最后一条视为掉电撕裂，故意丢弃。 */
}

esp_err_t memory_store_restore(memory_state_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;
    esp_err_t err = memory_profile_restore(state);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "恢复画像快照失败: %s", esp_err_to_name(err));
    }
    err = read_lines(MEMORY_ENGINE_EVENT_PATH, restore_event_line, state);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "恢复事实日志失败: %s", esp_err_to_name(err));
        return err;
    }
    err = read_lines(MEMORY_ENGINE_TOMBSTONE_PATH, restore_tombstone_line, state);
    if (err != ESP_OK) ESP_LOGW(TAG, "恢复删除标记失败: %s", esp_err_to_name(err));
    return err;
}

esp_err_t memory_store_append_event(const memory_record_t *record)
{
    if (!record || !memory_type_name(record->type)) return ESP_ERR_INVALID_ARG;
    cJSON *json = cJSON_CreateObject();
    if (!json || !cJSON_AddNumberToObject(json, "v", 1)
        || !cJSON_AddNumberToObject(json, "id", record->id)
        || !cJSON_AddStringToObject(json, "type", memory_type_name(record->type))
        || !cJSON_AddStringToObject(json, "content", record->content)
        || !cJSON_AddNumberToObject(json, "importance", record->importance)
        || !cJSON_AddNumberToObject(json, "created_at", (double)record->created_at)
        || !cJSON_AddStringToObject(json, "source", "user")) {
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    bool expiry_added = record->expires_at
                        ? cJSON_AddNumberToObject(json, "expires_at", (double)record->expires_at)
                        : cJSON_AddNullToObject(json, "expires_at");
    if (!expiry_added) {
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    return append_json(MEMORY_ENGINE_EVENT_PATH, json);
}

esp_err_t memory_store_append_tombstone(memory_id_t id)
{
    if (id == MEMORY_ID_INVALID) return ESP_ERR_INVALID_ARG;
    cJSON *json = cJSON_CreateObject();
    if (!json || !cJSON_AddNumberToObject(json, "v", 1)
        || !cJSON_AddNumberToObject(json, "id", id)
        || !cJSON_AddNumberToObject(json, "deleted_at", (double)time(NULL))) {
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    return append_json(MEMORY_ENGINE_TOMBSTONE_PATH, json);
}
