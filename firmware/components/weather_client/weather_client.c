#include "weather_client.h"

#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"

static const char *TAG = "weather_client";

#define POLL_INTERVAL_MS (15 * 60 * 1000) /* 天气变化慢，桥接服务自己也是15分钟刷新一次 */
#define MDNS_QUERY_TIMEOUT_MS 3000
#define HTTP_TIMEOUT_MS  5000
#define RESP_BUF_SIZE    512 /* /api/weather 响应很小，静态缓冲足够 */

static char s_host[64];
static uint16_t s_port;

static weather_now_t s_weather = { 0 };
static SemaphoreHandle_t s_lock = NULL;

static char s_resp_buf[RESP_BUF_SIZE];
static int s_resp_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    int remain = RESP_BUF_SIZE - 1 - s_resp_len;
    if (remain <= 0) {
        return ESP_OK;
    }
    int n = evt->data_len < remain ? evt->data_len : remain;
    memcpy(s_resp_buf + s_resp_len, evt->data, n);
    s_resp_len += n;
    s_resp_buf[s_resp_len] = '\0';
    return ESP_OK;
}

/* mDNS解析桥接服务主机名 -> IPv4文本地址，写入out(需至少16字节) */
static esp_err_t resolve_host(char *out, size_t out_len)
{
    esp_ip4_addr_t addr = { 0 };
    esp_err_t err = mdns_query_a(s_host, MDNS_QUERY_TIMEOUT_MS, &addr);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(out, out_len, IPSTR, IP2STR(&addr));
    return ESP_OK;
}

static float json_num(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? (float)item->valuedouble : 0.0f;
}

static void json_str(cJSON *obj, const char *key, char *out, size_t out_len)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    const char *s = cJSON_IsString(item) ? item->valuestring : "";
    strncpy(out, s, out_len - 1);
    out[out_len - 1] = '\0';
}

static esp_err_t parse_and_store(const char *json_str_body)
{
    cJSON *root = cJSON_Parse(json_str_body);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *available = cJSON_GetObjectItem(root, "available");
    if (!cJSON_IsBool(available) || !cJSON_IsTrue(available)) {
        /* 桥接服务明确返回 available:false(如未配置qweather_*)，不是解析错误，按"暂不可用"处理 */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_weather.available = false;
        xSemaphoreGive(s_lock);
        cJSON_Delete(root);
        return ESP_OK;
    }

    char wind_dir[16], wind_scale[8];
    json_str(root, "wind_dir", wind_dir, sizeof(wind_dir));
    json_str(root, "wind_scale", wind_scale, sizeof(wind_scale));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_weather.available = true;
    s_weather.temperature = json_num(root, "temp");
    s_weather.humidity = (int)json_num(root, "humidity");
    json_str(root, "condition", s_weather.condition, sizeof(s_weather.condition));
    snprintf(s_weather.wind, sizeof(s_weather.wind), "%s%s级", wind_dir, wind_scale);
    s_weather.fetched_at = (time_t)json_num(root, "fetched_at");
    xSemaphoreGive(s_lock);

    cJSON_Delete(root);
    return ESP_OK;
}

static void poll_once(void)
{
    char ip[16];
    esp_err_t err = resolve_host(ip, sizeof(ip));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS解析%s.local失败: %s", s_host, esp_err_to_name(err));
        return;
    }

    char url[80];
    snprintf(url, sizeof(url), "http://%s:%u/api/weather", ip, (unsigned)s_port);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    s_resp_len = 0;
    s_resp_buf[0] = '\0';

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "轮询失败: err=%s status=%d", esp_err_to_name(err), status);
        return;
    }

    if (parse_and_store(s_resp_buf) != ESP_OK) {
        ESP_LOGW(TAG, "响应解析失败: %s", s_resp_buf);
    }
}

static void weather_task(void *arg)
{
    (void)arg;
    for (;;) {
        poll_once();
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

esp_err_t weather_client_init(const char *mdns_host, uint16_t port)
{
    if (!mdns_host || !mdns_host[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(s_host, mdns_host, sizeof(s_host) - 1);
    s_host[sizeof(s_host) - 1] = '\0';
    s_port = port;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    /* mdns_init 是幂等的(ESP_ERR_INVALID_STATE表示已由claude_quota_client初始化过)，
     * 两个客户端共用同一个桥接服务地址，各自独立轮询任务 */
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mdns_init失败: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreate(weather_task, "weather_task", 4096, NULL, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "init OK, 目标=%s.local:%u, 轮询间隔%dmin", s_host, s_port, POLL_INTERVAL_MS / 60000);
    return ESP_OK;
}

bool weather_client_get(weather_now_t *out)
{
    if (!out || !s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_weather;
    xSemaphoreGive(s_lock);
    return s_weather.fetched_at != 0;
}
