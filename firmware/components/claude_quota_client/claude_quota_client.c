#include "claude_quota_client.h"

#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "sys_state.h"

static const char *TAG = "quota_client";

#define POLL_INTERVAL_MS        (60 * 1000)
#define POLL_INTERVAL_BATTERY_MS (300 * 1000) /* 电池供电(未接USB)时降频到300s */
#define MDNS_QUERY_TIMEOUT_MS 3000
#define HTTP_TIMEOUT_MS    5000
#define RESP_BUF_SIZE      512 /* /api/quota/compact 响应很小，静态缓冲足够 */

static char s_host[64];
static uint16_t s_port;

static claude_quota_t s_quota = { 0 };
static SemaphoreHandle_t s_lock = NULL;

static char s_resp_buf[RESP_BUF_SIZE];
static int s_resp_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    /* 累积响应体，超出缓冲区的部分丢弃(compact接口不该超过512字节，超了说明协议错了) */
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


/* 桥接不可达只降级本页；保留旧数值供页面标“旧”，不影响其他智能体能力。 */
static void mark_unavailable(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_quota.available = false;
    sys_state_apply_quota(false, s_quota.used_5h, s_quota.used_7d,
                          s_quota.resets_5h, s_quota.resets_7d, s_quota.fetched_at);
    xSemaphoreGive(s_lock);
}

/* bridge 的用量字段是0~1的小数；拒绝截断/非数值时间，避免异常响应污染快照。 */
static bool quota_fraction_valid(const cJSON *item)
{
    return cJSON_IsNumber(item) && item->valuedouble >= 0 && item->valuedouble <= 1;
}

static bool quota_epoch_valid(const cJSON *item)
{
    return cJSON_IsNumber(item) && item->valuedouble > 0 && item->valuedouble <= 4102444800.0
           && item->valuedouble == (double)(time_t)item->valuedouble;
}
/* 解析 /api/quota/compact 响应体，成功则更新 s_quota(加锁)，失败返回错误码不改动缓存 */
static esp_err_t parse_and_store(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *available = cJSON_GetObjectItem(root, "available");
    cJSON *fetched_at = cJSON_GetObjectItem(root, "fetched_at");
    cJSON *h5 = cJSON_GetObjectItem(root, "5h");
    cJSON *d7 = cJSON_GetObjectItem(root, "7d");
    bool ok = cJSON_IsBool(available) && cJSON_IsTrue(available)
            && cJSON_IsObject(h5) && cJSON_IsObject(d7)
            && quota_fraction_valid(cJSON_GetObjectItem(h5, "used"))
            && quota_epoch_valid(cJSON_GetObjectItem(h5, "resets_at"))
            && quota_fraction_valid(cJSON_GetObjectItem(d7, "used"))
            && quota_epoch_valid(cJSON_GetObjectItem(d7, "resets_at"))
            && quota_epoch_valid(fetched_at);
    if (!ok) {
        /* available:false与畸形响应都只降级额度页，旧缓存仍可供页面标“旧”。 */
        mark_unavailable();
        cJSON_Delete(root);
        return ESP_OK;
    }
    cJSON *h5_used = cJSON_GetObjectItem(h5, "used");
    cJSON *h5_resets = cJSON_GetObjectItem(h5, "resets_at");
    cJSON *d7_used = cJSON_GetObjectItem(d7, "used");
    cJSON *d7_resets = cJSON_GetObjectItem(d7, "resets_at");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_quota.available = true;
    s_quota.used_5h = (float)cJSON_GetNumberValue(h5_used);
    s_quota.resets_5h = (time_t)cJSON_GetNumberValue(h5_resets);
    s_quota.used_7d = (float)cJSON_GetNumberValue(d7_used);
    s_quota.resets_7d = (time_t)cJSON_GetNumberValue(d7_resets);
    s_quota.fetched_at = (time_t)cJSON_GetNumberValue(fetched_at);
    sys_state_apply_quota(true, s_quota.used_5h, s_quota.used_7d,
                          s_quota.resets_5h, s_quota.resets_7d, s_quota.fetched_at);
    xSemaphoreGive(s_lock);
    cJSON_Delete(root);
    return ESP_OK;
}

static void poll_once(void)
{
    char ip[16];
    esp_err_t err = resolve_host(ip, sizeof(ip));
    if (err != ESP_OK) {
        mark_unavailable();
        ESP_LOGW(TAG, "mDNS解析%s.local失败: %s", s_host, esp_err_to_name(err));
        return;
    }

    char url[80];
    snprintf(url, sizeof(url), "http://%s:%u/api/quota/compact", ip, (unsigned)s_port);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGW(TAG, "HTTP客户端创建失败");
        mark_unavailable();
        return;
    }
    s_resp_len = 0;
    s_resp_buf[0] = '\0';

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "轮询失败: err=%s status=%d", esp_err_to_name(err), status);
        mark_unavailable();
        return;
    }

    if (parse_and_store(s_resp_buf) != ESP_OK) {
        ESP_LOGW(TAG, "响应解析失败: %s", s_resp_buf);
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

static void quota_task(void *arg)
{
    (void)arg;
    for (;;) {
        poll_once();
        vTaskDelay(pdMS_TO_TICKS(poll_delay_ms()));
    }
}

esp_err_t claude_quota_client_init(const char *mdns_host, uint16_t port)
{
    if (s_lock) return ESP_OK;
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

    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE /* 已初始化过 */) {
        ESP_LOGW(TAG, "mdns_init失败: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return err;
    }

    BaseType_t ok = xTaskCreate(quota_task, "claude_quota_task", 4096, NULL, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "任务创建失败");
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "init OK, 目标=%s.local:%u, 轮询间隔%ds", s_host, s_port, POLL_INTERVAL_MS / 1000);
    return ESP_OK;
}

bool claude_quota_client_get(claude_quota_t *out)
{
    if (!out || !s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_quota;
    bool has_data = s_quota.fetched_at != 0;
    xSemaphoreGive(s_lock);
    return has_data;
}
