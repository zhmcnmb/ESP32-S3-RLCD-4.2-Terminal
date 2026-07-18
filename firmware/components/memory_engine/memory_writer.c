/* memory_writer.c - 把热路径SD写收敛到有界PSRAM队列与单任务，避免阻塞agent。
 *
 * 为什么独立任务：原先record/forget/maintain在s_lock持有期间同步调用storage_sd，
 * 一次FATFS操作可能数十毫秒，直接阻塞agent任务。改为入队后由专用任务串行落盘，
 * 事件日志是事实源，重启回放兜底，写失败仅告警不重试。 */
#include "memory_engine_internal.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "storage_sd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "memory_writer";

#define WRITER_QUEUE_DEPTH   8
#define WRITER_TASK_STACK    4096
#define WRITER_TASK_PRIORITY 2

/* 操作码：0=追加JSONL行，1=原子替换文件 */
enum { WRITER_OP_APPEND = 0, WRITER_OP_REPLACE = 1 };

typedef struct {
    uint8_t  op;
    char     path[64];
    uint16_t len;
    char     payload[MEMORY_ENGINE_PROFILE_FILE_MAX];
} writer_item_t;

static QueueHandle_t s_queue;
/* 两个约2KB暂存项放PSRAM；入队者由memory_engine状态锁串行，写任务独占接收项。 */
static writer_item_t *s_enqueue_item;
static writer_item_t *s_worker_item;
/* 追加失败后内存状态可能含未落盘事实；仅重启回放能重建一致状态，期间禁止更新画像槽。 */
static bool s_event_log_consistent;

/* 写任务：从队列取项串行落盘，失败仅告警不重试——事件日志是事实源。 */
static void memory_store_task(void *arg)
{
    (void)arg;
    while (xQueueReceive(s_queue, s_worker_item, portMAX_DELAY) == pdTRUE) {
        writer_item_t *item = s_worker_item;
        esp_err_t err;
        if (item->op == WRITER_OP_APPEND) {
            item->payload[item->len] = '\0';
            err = storage_sd_append_line(item->path, item->payload);
        } else if (!s_event_log_consistent) {
            ESP_LOGW(TAG, "事实日志未落盘，跳过画像快照");
            continue;
        } else {
            err = storage_sd_atomic_replace(item->path, item->payload, item->len);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "写入失败 %s: %s", item->path, esp_err_to_name(err));
            if (item->op == WRITER_OP_APPEND) s_event_log_consistent = false;
        }
    }
    vTaskDelete(NULL);
}
static void release_scratch_items(void)
{
    heap_caps_free(s_enqueue_item);
    heap_caps_free(s_worker_item);
    s_enqueue_item = NULL;
    s_worker_item = NULL;
}

esp_err_t memory_writer_start(void)
{
    if (s_queue) return ESP_OK; /* 幂等：重复启动安全 */
    s_enqueue_item = heap_caps_calloc(1, sizeof(*s_enqueue_item), MALLOC_CAP_SPIRAM);
    s_worker_item = heap_caps_calloc(1, sizeof(*s_worker_item), MALLOC_CAP_SPIRAM);
    if (!s_enqueue_item || !s_worker_item) {
        ESP_LOGW(TAG, "暂存项分配失败");
        release_scratch_items();
        return ESP_ERR_NO_MEM;
    }
    s_event_log_consistent = true;
    /* 队列放PSRAM（~17KB），避免占用宝贵的内部SRAM。 */
    s_queue = xQueueCreateWithCaps(WRITER_QUEUE_DEPTH, sizeof(writer_item_t),
                                   MALLOC_CAP_SPIRAM);
    if (!s_queue) {
        ESP_LOGW(TAG, "队列创建失败");
        release_scratch_items();
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(memory_store_task, "memory_store", WRITER_TASK_STACK, NULL,
                    WRITER_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGW(TAG, "任务创建失败");
        vQueueDeleteWithCaps(s_queue);
        s_queue = NULL;
        release_scratch_items();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "memory_store_task 已启动");
    return ESP_OK;
}

/* 零等待入队：队列满即返回繁忙，调用方决定降级 */
static esp_err_t enqueue_item(const writer_item_t *item)
{
    if (!s_queue || !item) return ESP_ERR_INVALID_STATE;
    return xQueueSend(s_queue, item, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t set_item_path(writer_item_t *item, const char *rel_path)
{
    size_t path_len = strlen(rel_path);
    if (path_len >= sizeof(item->path)) return ESP_ERR_INVALID_SIZE;
    memcpy(item->path, rel_path, path_len + 1);
    return ESP_OK;
}

esp_err_t memory_writer_enqueue_line(const char *rel_path, const char *line)
{
    if (!rel_path || !line) return ESP_ERR_INVALID_ARG;
    size_t len = strlen(line);
    if (len >= sizeof(((writer_item_t *)0)->payload)) return ESP_ERR_INVALID_SIZE;
    if (!s_enqueue_item) return ESP_ERR_INVALID_STATE;
    /* memory_engine状态锁覆盖此段；队列复制完成前不得复用暂存项。 */
    memset(s_enqueue_item, 0, sizeof(*s_enqueue_item));
    esp_err_t err = set_item_path(s_enqueue_item, rel_path);
    if (err != ESP_OK) return err;
    s_enqueue_item->op = WRITER_OP_APPEND;
    s_enqueue_item->len = (uint16_t)len;
    memcpy(s_enqueue_item->payload, line, len);
    return enqueue_item(s_enqueue_item);
}

esp_err_t memory_writer_enqueue_replace(const char *rel_path, const void *data,
                                        size_t len)
{
    if (!rel_path || !data || len > MEMORY_ENGINE_PROFILE_FILE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_enqueue_item) return ESP_ERR_INVALID_STATE;
    /* 同上：画像快照写入在memory_engine状态锁内串行提交。 */
    memset(s_enqueue_item, 0, sizeof(*s_enqueue_item));
    esp_err_t err = set_item_path(s_enqueue_item, rel_path);
    if (err != ESP_OK) return err;
    s_enqueue_item->op = WRITER_OP_REPLACE;
    s_enqueue_item->len = (uint16_t)len;
    memcpy(s_enqueue_item->payload, data, len);
    return enqueue_item(s_enqueue_item);
}
