/* memory_engine.c - 板载长期记忆的策略与生命周期。 */
#include "memory_engine_internal.h"

#include <limits.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_log.h"
#include "storage_sd.h"
#include "time_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "memory_engine";
static memory_state_t s_state;
static SemaphoreHandle_t s_lock;

static bool ensure_lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    return s_lock != NULL;
}

static void lock_state(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock_state(void)
{
    xSemaphoreGive(s_lock);
}

const char *memory_type_name(memory_type_t type)
{
    static const char *const names[] = {"profile", "preference", "fact", "correction", "episode"};
    return type >= MEMORY_TYPE_PROFILE && type <= MEMORY_TYPE_EPISODE ? names[type] : NULL;
}

bool memory_type_parse(const char *name, memory_type_t *out_type)
{
    if (!name || !out_type) return false;
    for (int type = MEMORY_TYPE_PROFILE; type <= MEMORY_TYPE_EPISODE; type++) {
        if (strcmp(name, memory_type_name((memory_type_t)type)) == 0) {
            *out_type = (memory_type_t)type;
            return true;
        }
    }
    return false;
}

uint8_t memory_type_importance(memory_type_t type)
{
    switch (type) {
    case MEMORY_TYPE_PROFILE: return 95;
    case MEMORY_TYPE_CORRECTION: return 90;
    case MEMORY_TYPE_PREFERENCE: return 80;
    case MEMORY_TYPE_FACT: return 70;
    case MEMORY_TYPE_EPISODE: return 50;
    default: return 0;
    }
}

static bool utf8_valid(const char *text)
{
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor;) {
        size_t extra = *cursor < 0x80 ? 0 : (*cursor & 0xe0) == 0xc0 ? 1
                       : (*cursor & 0xf0) == 0xe0 ? 2 : (*cursor & 0xf8) == 0xf0 ? 3 : SIZE_MAX;
        if (extra == SIZE_MAX) return false;
        for (size_t i = 1; i <= extra; i++) {
            if (cursor[i] == '\0' || (cursor[i] & 0xc0) != 0x80) return false;
        }
        cursor += extra + 1;
    }
    return true;
}

static bool contains_sensitive_data(const char *content)
{
    static const char *const terms[] = {
        "密码", "口令", "身份证", "银行卡", "私钥", "token", "api key", "secret", "cvv",
    };
    for (size_t i = 0; i < sizeof(terms) / sizeof(terms[0]); i++) {
        if (strcasestr(content, terms[i])) return true;
    }
    return false;
}

static bool valid_event(const memory_event_t *event)
{
    if (!event || !memory_type_name(event->type) || !event->content || !event->content[0]
        || strlen(event->content) > MEMORY_ENGINE_CONTENT_MAX
        || event->ttl_seconds > MEMORY_ENGINE_TTL_MAX_SECONDS) return false;
    return utf8_valid(event->content) && !contains_sensitive_data(event->content);
}

static size_t find_record(memory_id_t id)
{
    for (size_t i = 0; i < s_state.count; i++) {
        if (s_state.entries[i].record.id == id) return i;
    }
    return SIZE_MAX;
}

static bool duplicate_event(const memory_event_t *event, time_t now)
{
    for (size_t i = 0; i < s_state.count; i++) {
        const memory_record_t *record = &s_state.entries[i].record;
        if (!memory_record_expired(record, now) && record->type == event->type
            && strcmp(record->content, event->content) == 0) return true;
    }
    return false;
}

static size_t active_profile_count(time_t now)
{
    size_t count = 0;
    for (size_t i = 0; i < s_state.count; i++) {
        const memory_record_t *record = &s_state.entries[i].record;
        if (record->type == MEMORY_TYPE_PROFILE && !memory_record_expired(record, now)) count++;
    }
    return count;
}

static void update_profile_snapshot(void)
{
    esp_err_t err = memory_profile_write(&s_state);
    if (err != ESP_OK) ESP_LOGW(TAG, "更新画像快照失败: %s", esp_err_to_name(err));
}

esp_err_t memory_engine_init(void)
{
    if (!ensure_lock()) return ESP_ERR_NO_MEM;
    lock_state();
    if (s_state.initialized) {
        unlock_state();
        return ESP_OK;
    }
    memset(&s_state, 0, sizeof(s_state));
    s_state.initialized = true;
    s_state.next_id = 1;
    if (!storage_sd_is_mounted()) {
        ESP_LOGW(TAG, "SD不可用，长期记忆已禁用");
        unlock_state();
        return ESP_OK;
    }
    esp_err_t err = memory_store_restore(&s_state);
    if (err != ESP_OK) {
        memset(&s_state, 0, sizeof(s_state));
        unlock_state();
        return err;
    }
    /* 恢复完成后再启动writer；启动失败视同SD缺失降级 */
    esp_err_t werr = memory_writer_start();
    if (werr != ESP_OK) {
        ESP_LOGW(TAG, "writer启动失败，记忆降级: %s", esp_err_to_name(werr));
        /* available保持false，短会话不受影响 */
    } else {
        s_state.available = true;
        ESP_LOGI(TAG, "已恢复%u条长期记忆", (unsigned)s_state.count);
    }
    unlock_state();
    return ESP_OK;
}

bool memory_engine_is_available(void)
{
    if (!s_lock) return false;
    lock_state();
    bool available = s_state.initialized && s_state.available;
    unlock_state();
    return available;
}

esp_err_t memory_recall(const memory_query_t *query, memory_context_t *out)
{
    if (!query || !out || !s_lock) return ESP_ERR_INVALID_STATE;
    lock_state();
    esp_err_t err = !s_state.initialized || !s_state.available ? ESP_ERR_INVALID_STATE
                                                                 : memory_recall_build(&s_state, query, out);
    unlock_state();
    return err;
}

esp_err_t memory_record(const memory_event_t *event)
{
    if (!valid_event(event)) return ESP_ERR_INVALID_ARG;
    if (event->ttl_seconds && !time_service_is_valid()) return ESP_ERR_INVALID_STATE;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    lock_state();
    if (!s_state.initialized || !s_state.available) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    time_t now = time(NULL);
    if (duplicate_event(event, now)) {
        unlock_state();
        return ESP_OK;
    }
    if (s_state.count >= MEMORY_ENGINE_RECORD_CAPACITY || s_state.next_id == MEMORY_ID_INVALID
        || (event->type == MEMORY_TYPE_PROFILE
            && active_profile_count(now) >= MEMORY_ENGINE_PROFILE_CAPACITY)) {
        unlock_state();
        return ESP_ERR_INVALID_SIZE;
    }
    memory_record_t record = {
        .id = s_state.next_id, .type = event->type, .importance = memory_type_importance(event->type),
        .created_at = now, .expires_at = event->ttl_seconds ? now + (time_t)event->ttl_seconds : 0,
    };
    strcpy(record.content, event->content);
    esp_err_t err = memory_store_append_event(&record);
    if (err == ESP_OK) {
        s_state.entries[s_state.count++].record = record;
        s_state.next_id = record.id == UINT32_MAX ? MEMORY_ID_INVALID : record.id + 1;
        if (record.type == MEMORY_TYPE_PROFILE) update_profile_snapshot();
    }
    unlock_state();
    return err;
}

esp_err_t memory_forget(memory_id_t id)
{
    if (id == MEMORY_ID_INVALID || !s_lock) return ESP_ERR_INVALID_STATE;
    lock_state();
    if (!s_state.initialized || !s_state.available) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    size_t index = find_record(id);
    if (index == SIZE_MAX) {
        unlock_state();
        return ESP_ERR_NOT_FOUND;
    }
    bool profile = s_state.entries[index].record.type == MEMORY_TYPE_PROFILE;
    esp_err_t err = memory_store_append_tombstone(id);
    if (err == ESP_OK) {
        memory_erase_entry(&s_state, index);
        if (profile) update_profile_snapshot();
    }
    unlock_state();
    return err;
}
