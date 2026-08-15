#include "sys_state.h"

#include <string.h>
#include <time.h>

#include "battery.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net_manager.h"
#include "sensor_shtc3.h"
#include "storage_sd.h"
#include "time_service.h"


#define POLL_INTERVAL_MS (10 * 1000) /* 与迁移前page_home的SENSOR_REFRESH_TICKS(约10s)保持一致 */

static sys_state_snapshot_t s_state;
static SemaphoreHandle_t s_lock;
static bool s_voice_cancel_req = false;
static bool s_voice_trigger_req = false;
static sys_wifi_cmd_t s_wifi_cmd = SYS_WIFI_CMD_NONE; /* NONE=空槽 */
static char s_reminder[SYS_STATE_REMINDER_TEXT_MAX]; /* 空串=无待播报提醒 */
static char s_text_turn[SYS_STATE_TEXT_TURN_MAX];    /* 空串=无待处理文本轮 */

/* 所有apply_*()共用的收尾: 持锁写完字段后统一在这里递增版本号，
 * 避免每个apply函数各自记一遍，也不会漏掉 */
static void bump_version_locked(void)
{
    s_state.version++;
}

void sys_state_get_snapshot(sys_state_snapshot_t *out)
{
    if (!out || !s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
    /* 时间开销极低且没有专属后台任务，直接现读，永远反映"此刻" */
    out->time_valid = time_service_is_valid();
    out->now = time(NULL);
}

static void sys_state_apply_env(bool ok, float temperature, float humidity, time_t updated_at)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.env_ok = ok;
    s_state.temperature = ok ? temperature : 0;
    s_state.humidity = ok ? humidity : 0;
    s_state.env_updated_at = updated_at;
    bump_version_locked();
    xSemaphoreGive(s_lock);
}

static void sys_state_apply_battery(bool ok, float voltage, int percent, bool charging)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.battery_ok = ok;
    s_state.battery_voltage = ok ? voltage : 0;
    s_state.battery_percent = ok ? percent : 0;
    s_state.battery_charging = ok && charging;
    bump_version_locked();
    xSemaphoreGive(s_lock);
}

static void sys_state_apply_wifi(bool connected)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.wifi_connected = connected;
    bump_version_locked();
    xSemaphoreGive(s_lock);
}

static void sys_state_apply_sd(bool mounted)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.sd_mounted = mounted;
    bump_version_locked();
    xSemaphoreGive(s_lock);
}

void sys_state_apply_env_history(const sys_state_env_sample_t *samples, int count, uint32_t total)
{
    if (!s_lock || !samples) {
        return;
    }
    if (count < 0) {
        count = 0;
    }
    if (count > SYS_STATE_ENV_HISTORY_LEN) {
        count = SYS_STATE_ENV_HISTORY_LEN;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_state.env_history, samples, count * sizeof(sys_state_env_sample_t));
    s_state.env_history_count = count;
    s_state.env_sample_total = total;
    bump_version_locked();
    xSemaphoreGive(s_lock);
}

void sys_state_apply_weather(bool available, float temperature, int humidity,
                             const char *condition, const char *wind, time_t updated_at)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.weather_available = available;
    s_state.weather_temperature = temperature;
    s_state.weather_humidity = humidity;
    if (condition) {
        strncpy(s_state.weather_condition, condition, sizeof(s_state.weather_condition) - 1);
        s_state.weather_condition[sizeof(s_state.weather_condition) - 1] = '\0';
    } else {
        s_state.weather_condition[0] = '\0';
    }
    if (wind) {
        strncpy(s_state.weather_wind, wind, sizeof(s_state.weather_wind) - 1);
        s_state.weather_wind[sizeof(s_state.weather_wind) - 1] = '\0';
    } else {
        s_state.weather_wind[0] = '\0';
    }
    s_state.weather_updated_at = updated_at;
    bump_version_locked();
    xSemaphoreGive(s_lock);
}


void sys_state_apply_voice(sys_voice_phase_t phase, uint32_t turn_id, const char *reason)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.voice_phase = phase;
    s_state.voice_turn_id = turn_id;
    if (phase == SYS_VOICE_ERROR && reason) {
        strncpy(s_state.voice_error, reason, sizeof(s_state.voice_error) - 1);
        s_state.voice_error[sizeof(s_state.voice_error) - 1] = '\0';
    } else {
        s_state.voice_error[0] = '\0';
    }
    bump_version_locked();
    xSemaphoreGive(s_lock);
}

void sys_state_apply_wifi_setup(const sys_wifi_setup_view_t *view)
{
    if (!s_lock || !view) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.wifi_setup = *view;
    bump_version_locked();
    xSemaphoreGive(s_lock);
}

void sys_state_post_wifi_cmd(sys_wifi_cmd_t cmd)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_wifi_cmd = cmd;
    xSemaphoreGive(s_lock);
}

sys_wifi_cmd_t sys_state_consume_wifi_cmd(void)
{
    if (!s_lock) {
        return SYS_WIFI_CMD_NONE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sys_wifi_cmd_t cmd = s_wifi_cmd;
    s_wifi_cmd = SYS_WIFI_CMD_NONE;
    xSemaphoreGive(s_lock);
    return cmd;
}

void sys_state_request_voice_cancel(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_voice_cancel_req = true;
    xSemaphoreGive(s_lock);
}

bool sys_state_consume_voice_cancel(void)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool req = s_voice_cancel_req;
    s_voice_cancel_req = false;
    xSemaphoreGive(s_lock);
    return req;
}

void sys_state_request_voice_trigger(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_voice_trigger_req = true;
    xSemaphoreGive(s_lock);
}

bool sys_state_consume_voice_trigger(void)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool req = s_voice_trigger_req;
    s_voice_trigger_req = false;
    xSemaphoreGive(s_lock);
    return req;
}

bool sys_state_post_reminder(const char *text)
{
    if (!s_lock || !text || !text[0]) return false;
    size_t len = strlen(text);
    if (len >= sizeof(s_reminder)) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool accepted = s_reminder[0] == '\0';
    if (accepted) memcpy(s_reminder, text, len + 1);
    xSemaphoreGive(s_lock);
    return accepted;
}

bool sys_state_reminder_pending(void)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool pending = s_reminder[0] != '\0';
    xSemaphoreGive(s_lock);
    return pending;
}

bool sys_state_consume_reminder(char *out, size_t out_len)
{
    if (!s_lock || !out || out_len == 0) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t len = strlen(s_reminder);
    bool consumed = len > 0 && len < out_len;
    if (consumed) {
        memcpy(out, s_reminder, len + 1);
        s_reminder[0] = '\0';
    }
    xSemaphoreGive(s_lock);
    return consumed;
}

bool sys_state_post_text_turn(const char *text)
{
    if (!s_lock || !text || !text[0]) return false;
    size_t len = strlen(text);
    if (len >= sizeof(s_text_turn)) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool accepted = s_text_turn[0] == '\0';
    if (accepted) memcpy(s_text_turn, text, len + 1);
    xSemaphoreGive(s_lock);
    return accepted;
}

bool sys_state_consume_text_turn(char *out, size_t out_len)
{
    if (!s_lock || !out || out_len == 0) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t len = strlen(s_text_turn);
    bool consumed = len > 0 && len < out_len;
    if (consumed) {
        memcpy(out, s_text_turn, len + 1);
        s_text_turn[0] = '\0';
    }
    xSemaphoreGive(s_lock);
    return consumed;
}

/* 没有专属后台任务的"环境状态"由这里统一采集：瞬时温湿度、电池、WiFi连接、
 * SD挂载。和 env_logger(60s历史采样)、weather_client
 * (各自mDNS+HTTP轮询)相互独立，互不阻塞——本任务只做本地读取，没有网络IO */
static void poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        shtc3_reading_t env = {0};
        esp_err_t env_err = shtc3_read(&env);
        sys_state_apply_env(env_err == ESP_OK, env.temperature, env.humidity, time(NULL));

        battery_status_t bat = {0};
        esp_err_t bat_err = battery_read(&bat);
        sys_state_apply_battery(bat_err == ESP_OK, bat.voltage, bat.percent, bat.charging);

        sys_state_apply_wifi(net_get_state() == NET_CONNECTED);
        sys_state_apply_sd(storage_sd_is_mounted());

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

esp_err_t sys_state_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    BaseType_t ok = xTaskCreate(poll_task, "sys_state_task", 3072, NULL, 2, NULL);
    if (ok != pdPASS) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
