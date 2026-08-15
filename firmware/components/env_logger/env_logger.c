#include "env_logger.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor_shtc3.h"
#include "storage_sd.h"
#include "sys_state.h"

static const char *TAG = "env_logger";

#define SAMPLE_INTERVAL_MS (60 * 1000)
#define CSV_PATH "env.csv"
#define ENV_LOGGER_HISTORY_LEN 60 /* 60点 × 60s = 1小时趋势窗口 */

typedef struct {
    time_t timestamp;
    float temperature;
    float humidity;
} env_logger_sample_t;

static int get_history(env_logger_sample_t *out, int count);

static env_logger_sample_t s_ring[ENV_LOGGER_HISTORY_LEN];
static int s_ring_head = 0;  /* 下一个写入位置 */
static int s_ring_count = 0; /* 环形缓冲当前有效条数 */
static uint32_t s_total_count = 0;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* SD卡是可选外设，未挂载时静默跳过——不属于"错误"，不打日志刷屏 */
static void append_csv(const env_logger_sample_t *s)
{
    if (!storage_sd_is_mounted()) {
        return;
    }
    char line[64];
    snprintf(line, sizeof(line), "%lld,%.2f,%.2f",
              (long long)s->timestamp, s->temperature, s->humidity);
    esp_err_t err = storage_sd_append_line(CSV_PATH, line);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CSV写入失败: %s", esp_err_to_name(err));
    }
}

static void push_sample(const env_logger_sample_t *s)
{
    portENTER_CRITICAL(&s_lock);
    s_ring[s_ring_head] = *s;
    s_ring_head = (s_ring_head + 1) % ENV_LOGGER_HISTORY_LEN;
    if (s_ring_count < ENV_LOGGER_HISTORY_LEN) {
        s_ring_count++;
    }
    s_total_count++;
    portEXIT_CRITICAL(&s_lock);
}

/* 发布历史快照，趋势页不直接读取 env_logger。 */
static void publish_history_snapshot(void)
{
    static env_logger_sample_t hist[ENV_LOGGER_HISTORY_LEN];
    static sys_state_env_sample_t conv[ENV_LOGGER_HISTORY_LEN];

    int n = get_history(hist, ENV_LOGGER_HISTORY_LEN);
    for (int i = 0; i < n; i++) {
        conv[i].timestamp = hist[i].timestamp;
        conv[i].temperature = hist[i].temperature;
        conv[i].humidity = hist[i].humidity;
    }
    sys_state_apply_env_history(conv, n, s_total_count);
}

static void env_logger_task(void *arg)
{
    (void)arg;
    for (;;) {
        shtc3_reading_t reading;
        esp_err_t err = shtc3_read(&reading);
        if (err == ESP_OK) {
            env_logger_sample_t s = {
                .timestamp = time(NULL),
                .temperature = reading.temperature,
                .humidity = reading.humidity,
            };
            push_sample(&s);
            append_csv(&s);
            publish_history_snapshot();
        } else {
            ESP_LOGW(TAG, "采样失败: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
    }
}

esp_err_t env_logger_init(void)
{
    BaseType_t ok = xTaskCreate(env_logger_task, "env_logger_task", 4096, NULL, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "init OK, 采样间隔%ds, 历史窗口%d点",
             SAMPLE_INTERVAL_MS / 1000, ENV_LOGGER_HISTORY_LEN);
    return ESP_OK;
}

static int get_history(env_logger_sample_t *out, int count)
{
    if (!out || count <= 0) {
        return 0;
    }
    portENTER_CRITICAL(&s_lock);
    int n = (count < s_ring_count) ? count : s_ring_count;
    /* 环形缓冲最旧的一条在 (head - n) % LEN 位置 */
    int start = (s_ring_head - n + ENV_LOGGER_HISTORY_LEN) % ENV_LOGGER_HISTORY_LEN;
    for (int i = 0; i < n; i++) {
        out[i] = s_ring[(start + i) % ENV_LOGGER_HISTORY_LEN];
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

