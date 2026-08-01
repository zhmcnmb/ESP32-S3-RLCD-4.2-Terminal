/* scheduler.c - 一次/周期提醒、掉电恢复与单槽语音投递。 */
#include "scheduler.h"
#include <inttypes.h>

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sys_state.h"
#include "storage_sd.h"

static const char *TAG = "scheduler";

#define SCHEDULER_FILE_PATH "agent/tasks/tasks.json"
#define SCHEDULER_FILE_MAX 4096
#define SCHEDULER_TASK_STACK 4096
#define SCHEDULER_TASK_PRIORITY 3
#define SCHEDULER_TICK_MS 1000
#define TRUSTED_EPOCH 1700000000LL

typedef struct {
    bool active;
    scheduler_task_info_t info;
} scheduler_entry_t;

static scheduler_entry_t s_tasks[SCHEDULER_TASK_MAX];
static SemaphoreHandle_t s_lock;
static uint32_t s_next_id = 1;
static bool s_initialized;
static bool s_storage_warning_logged;
static bool s_time_was_trusted;
static time_t s_last_tick; /* 上次tick时间，检测NTP大幅回拨/前跳 */
static void warn_storage_once_locked(void)
{
    if (!s_storage_warning_logged) {
        ESP_LOGW(TAG, "SD不可用，提醒仅保留在RAM，重启后丢失");
        s_storage_warning_logged = true;
    }
}

static bool task_info_valid(const scheduler_task_info_t *info)
{
    return info->id != 0 && (info->kind == SCHED_ONCE || info->kind == SCHED_INTERVAL)
           && info->fire_at > 0 && info->text[0]
           && strlen(info->text) < sizeof(info->text)
           && (info->kind != SCHED_INTERVAL || info->interval_s > 0)
           && (info->kind != SCHED_ONCE || info->interval_s == 0);
}

static size_t active_count_locked(void)
{
    size_t count = 0;
    for (size_t i = 0; i < SCHEDULER_TASK_MAX; i++) {
        if (s_tasks[i].active) count++;
    }
    return count;
}

/* 紧凑键名把16条、每条96B且全部需转义的极端输入仍压在4KB原子替换上限内。 */
static bool encode_task(cJSON *array, const scheduler_task_info_t *info)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj || !cJSON_AddNumberToObject(obj, "i", info->id)
        || !cJSON_AddNumberToObject(obj, "k", info->kind)
        || !cJSON_AddNumberToObject(obj, "f", (double)info->fire_at)
        || !cJSON_AddNumberToObject(obj, "r", info->interval_s)
        || !cJSON_AddStringToObject(obj, "x", info->text)
        || !cJSON_AddItemToArray(array, obj)) {
        cJSON_Delete(obj);
        return false;
    }
    return true;
}

/* 调用方持有s_lock：持锁贯穿快照写入，避免两个并发变更以旧表覆盖新表。 */
static esp_err_t persist_locked(void)
{
    if (!storage_sd_is_mounted()) {
        warn_storage_once_locked();
        return ESP_OK;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *tasks = cJSON_CreateArray();
    if (!root || !tasks || !cJSON_AddNumberToObject(root, "n", s_next_id)
        || !cJSON_AddItemToObject(root, "t", tasks)) {
        cJSON_Delete(root);
        cJSON_Delete(tasks);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < SCHEDULER_TASK_MAX; i++) {
        if (s_tasks[i].active && !encode_task(tasks, &s_tasks[i].info)) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return ESP_ERR_NO_MEM;
    size_t len = strlen(text);
    esp_err_t err = len > SCHEDULER_FILE_MAX ? ESP_ERR_INVALID_SIZE
                                               : storage_sd_atomic_replace(SCHEDULER_FILE_PATH, text, len);
    free(text);
    return err;
}

/* SD故障时保留当前RAM任务可用，记录原因但不把文件系统故障伪装成调度失败。 */
static void persist_best_effort_locked(void)
{
    esp_err_t err = persist_locked();
    if (err != ESP_OK) ESP_LOGW(TAG, "提醒将仅保留在RAM: %s", esp_err_to_name(err));
}

static bool parse_task(const cJSON *obj, scheduler_task_info_t *out)
{
    cJSON *id = cJSON_GetObjectItemCaseSensitive(obj, "i");
    cJSON *kind = cJSON_GetObjectItemCaseSensitive(obj, "k");
    cJSON *fire_at = cJSON_GetObjectItemCaseSensitive(obj, "f");
    cJSON *interval = cJSON_GetObjectItemCaseSensitive(obj, "r");
    cJSON *text = cJSON_GetObjectItemCaseSensitive(obj, "x");
    if (!cJSON_IsNumber(id) || !cJSON_IsNumber(kind) || !cJSON_IsNumber(fire_at)
        || !cJSON_IsNumber(interval) || !cJSON_IsString(text) || !text->valuestring
        || id->valuedouble <= 0 || id->valuedouble > UINT32_MAX
        || id->valuedouble != (double)(uint32_t)id->valuedouble
        || (kind->valuedouble != SCHED_ONCE && kind->valuedouble != SCHED_INTERVAL)
        || interval->valuedouble < 0 || interval->valuedouble > UINT32_MAX
        || interval->valuedouble != (double)(uint32_t)interval->valuedouble
        || fire_at->valuedouble <= 0 || fire_at->valuedouble > 4102444800.0
        || fire_at->valuedouble != (double)(time_t)fire_at->valuedouble
        || strlen(text->valuestring) >= sizeof(out->text)) return false;
    memset(out, 0, sizeof(*out));
    out->id = (uint32_t)id->valuedouble;
    out->kind = (sched_kind_t)kind->valuedouble;
    out->fire_at = (time_t)fire_at->valuedouble;
    out->interval_s = (uint32_t)interval->valuedouble;
    strcpy(out->text, text->valuestring);
    return task_info_valid(out);
}

/* 已有可信时间时，恢复策略不补发长期过期任务，避免开机播出陈旧提醒。 */
static bool normalize_restored_task(scheduler_task_info_t *info, time_t now)
{
    if (now <= TRUSTED_EPOCH || info->fire_at > now) return true;
    if (info->kind == SCHED_ONCE) {
        if (now - info->fire_at < 24 * 60 * 60) return true;
        ESP_LOGW(TAG, "丢弃过期超过24h的一次提醒 id=%" PRIu32, info->id);
        return false;
    }
    uint64_t periods = (uint64_t)(now - info->fire_at) / info->interval_s + 1;
    info->fire_at += (time_t)(periods * info->interval_s);
    return true;
}

/* RTC掉电后延迟校时，须在时钟首次可信时重做恢复期的过期处理。 */
static bool normalize_after_clock_sync_locked(time_t now)
{
    bool changed = false;
    for (size_t i = 0; i < SCHEDULER_TASK_MAX; i++) {
        scheduler_entry_t *entry = &s_tasks[i];
        if (!entry->active) continue;
        time_t fire_at = entry->info.fire_at;
        if (!normalize_restored_task(&entry->info, now)) {
            entry->active = false;
            changed = true;
        } else if (entry->info.fire_at != fire_at) {
            changed = true;
        }
    }
    return changed;
}

/* 返回true表示恢复期丢弃或对齐了条目，调用方须回写干净快照。 */
static bool restore_entry_locked(const cJSON *item, time_t now, size_t *slot, uint32_t *max_id)
{
    scheduler_task_info_t info;
    if (!parse_task(item, &info)) return true;
    time_t stored_fire_at = info.fire_at;
    if (!normalize_restored_task(&info, now)) return true;
    for (size_t i = 0; i < *slot; i++) {
        if (s_tasks[i].info.id == info.id) return true;
    }
    if (*slot >= SCHEDULER_TASK_MAX) return true;
    scheduler_entry_t *entry = &s_tasks[(*slot)++];
    entry->active = true;
    entry->info = info;
    if (info.id > *max_id) *max_id = info.id;
    return info.fire_at != stored_fire_at;
}

/* 损坏/缺失文件以空任务表降级，不让持久化异常阻断普通语音与agent启动。 */
static bool restore_locked(void)
{
    if (!storage_sd_is_mounted()) {
        warn_storage_once_locked();
        return false;
    }
    uint32_t size = 0;
    esp_err_t err = storage_sd_get_file_size(SCHEDULER_FILE_PATH, &size);
    if (err == ESP_ERR_NOT_FOUND) return false;
    if (err != ESP_OK || size > SCHEDULER_FILE_MAX) {
        ESP_LOGW(TAG, "读取提醒表失败: %s", esp_err_to_name(err));
        return false;
    }
    char *text = heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM);
    if (!text) {
        ESP_LOGW(TAG, "恢复提醒表内存不足");
        return false;
    }
    int read = storage_sd_read_chunk(SCHEDULER_FILE_PATH, 0, text, size);
    if (read != (int)size) {
        ESP_LOGW(TAG, "读取提醒表不完整: %d/%u", read, (unsigned)size);
        free(text);
        return false;
    }
    text[size] = '\0';
    cJSON *root = cJSON_Parse(text);
    free(text);
    cJSON *tasks = root ? cJSON_GetObjectItemCaseSensitive(root, "t") : NULL;
    if (!root || !cJSON_IsArray(tasks)) {
        ESP_LOGW(TAG, "提醒表解析失败，按空表启动");
        cJSON_Delete(root);
        return false;
    }
    time_t now = time(NULL);
    uint32_t max_id = 0;
    size_t slot = 0;
    bool changed = false;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, tasks) {
        changed |= restore_entry_locked(item, now, &slot, &max_id);
    }
    cJSON *next = cJSON_GetObjectItemCaseSensitive(root, "n");
    bool next_valid = cJSON_IsNumber(next) && next->valuedouble > max_id
                      && next->valuedouble <= UINT32_MAX
                      && next->valuedouble == (double)(uint32_t)next->valuedouble;
    s_next_id = next_valid ? (uint32_t)next->valuedouble : max_id + 1;
    if (s_next_id == 0) s_next_id = 1;
    cJSON_Delete(root);
    return changed || !next_valid;
}

static void align_interval(scheduler_task_info_t *info, time_t now)
{
    uint64_t periods = (uint64_t)(now - info->fire_at) / info->interval_s + 1;
    info->fire_at += (time_t)(periods * info->interval_s);
}

static void process_due_tasks(void)
{
    if (!s_lock) return;
    time_t now = time(NULL);
    if (now <= TRUSTED_EPOCH) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_time_was_trusted) {
        s_time_was_trusted = true;
        if (normalize_after_clock_sync_locked(now)) persist_best_effort_locked();
    }
    /* NTP大幅回拨/前跳(>60s)时重新normalize，避免过期任务积压或间隔跑偏 */
    time_t diff = s_last_tick ? (now > s_last_tick ? now - s_last_tick : s_last_tick - now) : 0;
    if (diff > 60 && normalize_after_clock_sync_locked(now)) persist_best_effort_locked();
    for (size_t i = 0; i < SCHEDULER_TASK_MAX; i++) {
        scheduler_entry_t *entry = &s_tasks[i];
        if (!entry->active || entry->info.fire_at > now) continue;
        if (!sys_state_post_reminder(entry->info.text)) break;
        if (entry->info.kind == SCHED_ONCE) {
            entry->active = false;
        } else {
            align_interval(&entry->info, now);
        }
        persist_best_effort_locked();
        break; /* 邮箱单槽，一次最多成功投递一条 */
    }
    s_last_tick = now;
    xSemaphoreGive(s_lock);
}

static void scheduler_task(void *arg)
{
    (void)arg;
    for (;;) {
        process_due_tasks();
        vTaskDelay(pdMS_TO_TICKS(SCHEDULER_TICK_MS));
    }
}

esp_err_t scheduler_init(void)
{
    if (s_initialized) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_tasks, 0, sizeof(s_tasks));
    s_next_id = 1;
    s_time_was_trusted = time(NULL) > TRUSTED_EPOCH;
    bool restore_changed = restore_locked();
    if (restore_changed && s_time_was_trusted) persist_best_effort_locked();
    size_t restored = active_count_locked();
    xSemaphoreGive(s_lock);
    if (xTaskCreate(scheduler_task, "scheduler_task", SCHEDULER_TASK_STACK, NULL,
                    SCHEDULER_TASK_PRIORITY, NULL) != pdPASS) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "初始化完成，已恢复提醒%u条", (unsigned)restored);
    return ESP_OK;
}

bool scheduler_is_available(void)
{
    return s_initialized;
}

esp_err_t scheduler_add(sched_kind_t kind, uint32_t delay_s, uint32_t interval_s,
                        const char *text, uint32_t *out_id)
{
    if (!s_initialized || !text || !out_id || delay_s == 0 || (kind != SCHED_ONCE && kind != SCHED_INTERVAL)
        || (kind == SCHED_INTERVAL && interval_s == 0) || strlen(text) >= SCHEDULER_TEXT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    time_t now = time(NULL);
    if (now <= TRUSTED_EPOCH) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t slot = 0;
    while (slot < SCHEDULER_TASK_MAX && s_tasks[slot].active) slot++;
    if (slot == SCHEDULER_TASK_MAX) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    scheduler_task_info_t *info = &s_tasks[slot].info;
    uint32_t id = s_next_id;
    bool id_free = false;
    for (size_t tries = 0; tries <= SCHEDULER_TASK_MAX; tries++) {
        if (id == 0) id = 1;
        bool used = false;
        for (size_t i = 0; i < SCHEDULER_TASK_MAX; i++) {
            if (s_tasks[i].active && s_tasks[i].info.id == id) used = true;
        }
        if (!used) {
            id_free = true;
            break;
        }
        id = id == UINT32_MAX ? 1 : id + 1;
    }
    if (!id_free) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    *info = (scheduler_task_info_t) {
        .id = id, .kind = kind, .fire_at = now + delay_s,
        .interval_s = kind == SCHED_INTERVAL ? interval_s : 0,
    };
    s_next_id = id == UINT32_MAX ? 1 : id + 1;
    strcpy(info->text, text);
    s_tasks[slot].active = true;
    *out_id = info->id;
    persist_best_effort_locked();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t scheduler_cancel(uint32_t id)
{
    if (!s_initialized || id == 0) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < SCHEDULER_TASK_MAX; i++) {
        if (s_tasks[i].active && s_tasks[i].info.id == id) {
            s_tasks[i].active = false;
            persist_best_effort_locked();
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t scheduler_list(scheduler_task_info_t *out, size_t cap, size_t *out_count)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    size_t copied = 0;
    size_t total = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < SCHEDULER_TASK_MAX; i++) {
        if (!s_tasks[i].active) continue;
        total++;
        if (out && copied < cap) out[copied++] = s_tasks[i].info;
    }
    xSemaphoreGive(s_lock);
    if (out_count) *out_count = out ? copied : total;
    return out && total > cap ? ESP_ERR_INVALID_SIZE : ESP_OK;
}
