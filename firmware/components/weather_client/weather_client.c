#include "weather_client.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "cloud_transport.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sys_state.h"

static const char *TAG = "weather_client";

#define POLL_INTERVAL_MS (15 * 60 * 1000)      /* 天气变化慢，15分钟一次足够 */
#define POLL_INTERVAL_BATTERY_MS (30 * 60 * 1000) /* 电池供电(未接USB)时降频到30分钟 */
#define RESP_BUF_SIZE 1024 /* QWeather /now 响应解压后实测约450字节，留足余量 */
typedef struct {
    bool available;
    float temperature;
    int humidity;
    char condition[24];
    char wind[40];
    time_t fetched_at;
} weather_now_t;

static char s_api_key[64];
static char s_api_host[64];
static char s_location[32];

static weather_now_t s_weather = { 0 };
static SemaphoreHandle_t s_lock = NULL;

static void json_str(cJSON *obj, const char *key, char *out, size_t out_len)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    const char *s = cJSON_IsString(item) ? item->valuestring : "";
    strncpy(out, s, out_len - 1);
    out[out_len - 1] = '\0';
}

/* QWeather数值字段都是JSON字符串(如"32"、"63")，不是number类型 */
static float json_str_num(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) ? (float)atof(item->valuestring) : 0.0f;
}

/* 请求失败/解析失败时调用: 保留上次缓存值，只翻转available，让页面区分
 * "从未成功过"和"有旧数据可展示"(R0已确立的降级语义) */
static void mark_unavailable(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_weather.available = false;
    sys_state_apply_weather(false, s_weather.temperature, s_weather.humidity,
                            s_weather.condition, s_weather.wind, s_weather.fetched_at);
    xSemaphoreGive(s_lock);
}

static esp_err_t parse_and_store(const char *json_body)
{
    cJSON *root = cJSON_Parse(json_body);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *now = cJSON_GetObjectItem(root, "now");
    if (!cJSON_IsString(code) || strcmp(code->valuestring, "200") != 0 || !cJSON_IsObject(now)) {
        ESP_LOGW(TAG, "QWeather返回异常: code=%s", cJSON_IsString(code) ? code->valuestring : "?");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    char wind_dir[16], wind_scale[8];
    json_str(now, "windDir", wind_dir, sizeof(wind_dir));
    json_str(now, "windScale", wind_scale, sizeof(wind_scale));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_weather.available = true;
    s_weather.temperature = json_str_num(now, "temp");
    s_weather.humidity = (int)json_str_num(now, "humidity");
    json_str(now, "text", s_weather.condition, sizeof(s_weather.condition));
    snprintf(s_weather.wind, sizeof(s_weather.wind), "%s%s级", wind_dir, wind_scale);
    s_weather.fetched_at = time(NULL);
    sys_state_apply_weather(true, s_weather.temperature, s_weather.humidity,
                            s_weather.condition, s_weather.wind, s_weather.fetched_at);
    xSemaphoreGive(s_lock);

    cJSON_Delete(root);
    return ESP_OK;
}

static void poll_once(void)
{
    char url[128];
    snprintf(url, sizeof(url), "https://%s/v7/weather/now?location=%s", s_api_host, s_location);

    char resp[RESP_BUF_SIZE];
    cloud_transport_get_request_t request = {
        .url = url, .header_name = "X-QW-Api-Key", .header_value = s_api_key,
        .response = resp, .response_size = sizeof(resp),
    };
    esp_err_t err = cloud_transport_get(&request);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "请求失败: %s", esp_err_to_name(err));
        mark_unavailable();
        return;
    }

    if (parse_and_store(resp) != ESP_OK) {
        mark_unavailable();
    }
}

/* 电池供电(未接USB)时降低轮询频率省电；恢复USB供电自动回到正常档 */
static uint32_t poll_delay_ms(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);
    return (snap.battery_ok && !snap.battery_charging) ? POLL_INTERVAL_BATTERY_MS : POLL_INTERVAL_MS;
}

static void weather_task(void *arg)
{
    (void)arg;
    for (;;) {
        poll_once();
        vTaskDelay(pdMS_TO_TICKS(poll_delay_ms()));
    }
}

esp_err_t weather_client_init(const char *api_key, const char *api_host, const char *location)
{
    if (s_lock) return ESP_OK;
    if (!api_key || !api_key[0] || !api_host || !api_host[0] || !location || !location[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(s_api_key, api_key, sizeof(s_api_key) - 1);
    s_api_key[sizeof(s_api_key) - 1] = '\0';
    strncpy(s_api_host, api_host, sizeof(s_api_host) - 1);
    s_api_host[sizeof(s_api_host) - 1] = '\0';
    strncpy(s_location, location, sizeof(s_location) - 1);
    s_location[sizeof(s_location) - 1] = '\0';

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(weather_task, "weather_task", 8192, NULL, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "任务创建失败");
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "init OK, 目标=%s, 轮询间隔%dmin", s_api_host, POLL_INTERVAL_MS / 60000);
    return ESP_OK;
}

