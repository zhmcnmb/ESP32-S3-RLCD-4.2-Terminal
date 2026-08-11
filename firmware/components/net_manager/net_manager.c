#include "net_manager.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "net";

#define BIT_CONNECTED BIT0
#define MAX_RETRY 5
#define SLOW_RETRY_DELAY_US (10 * 1000 * 1000)

static EventGroupHandle_t s_events;
static net_state_t s_state = NET_DISCONNECTED;
static int s_retry = 0;
static esp_timer_handle_t s_retry_timer;
static net_connected_cb_t s_connected_cb = NULL;
static bool s_connected_once = false;

/* 慢速重连定时器回调：在esp_timer服务任务上下文执行，不占用事件循环任务。
 * esp_wifi_connect()只是把连接请求投进WiFi驱动内部队列，不在此处等待结果 */
static void on_retry_timer(void *arg)
{
    (void)arg;
    s_state = NET_CONNECTING;
    esp_wifi_connect();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_state = NET_CONNECTING;
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_state = NET_DISCONNECTED;
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        /* 无限重试会拖慢启动，但断线后需要自愈: 前MAX_RETRY次立即重连，
         * 之后放缓——放缓通过esp_timer单次定时器实现，事件回调本身绝不阻塞
         * (阻塞会卡住整个默认事件循环任务，波及所有WIFI/IP事件的派发) */
        if (s_retry < MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "disconnected, retry %d/%d", s_retry, MAX_RETRY);
            s_state = NET_CONNECTING;
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "disconnected, slow retry in %ds", SLOW_RETRY_DELAY_US / 1000000);
            esp_timer_start_once(s_retry_timer, SLOW_RETRY_DELAY_US);
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "connected, ip=" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry = 0;
        s_state = NET_CONNECTED;
        xEventGroupSetBits(s_events, BIT_CONNECTED);

        /* 只在首次连接成功时触发一次；断线重连不必重复执行(如SNTP已在跑) */
        if (!s_connected_once) {
            s_connected_once = true;
            if (s_connected_cb) {
                s_connected_cb();
            }
        }
    }
}

/* WiFi STA 启动序列：net_init 的中间段，拆出压函数红线 */
static esp_err_t wifi_start_sta(const char *ssid, const char *password)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    if (!esp_netif_create_default_wifi_sta()) {
        ESP_LOGE(TAG, "创建默认WiFi STA失败");
        return ESP_ERR_NO_MEM;
    }
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            on_wifi_event, NULL, NULL), TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            on_wifi_event, NULL, NULL), TAG, "ip handler");
    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "wifi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    return ESP_OK;
}

/* NVS保存的配网凭据读取，仅 net_init 使用 */
static esp_err_t net_credentials_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

esp_err_t net_init(const char *ssid, const char *password)
{
    ESP_RETURN_ON_FALSE(ssid && password, ESP_ERR_INVALID_ARG, TAG, "bad arg");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");

    s_events = xEventGroupCreate();
    if (!s_events) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = on_retry_timer,
        .name = "net_retry",
    };
    err = esp_timer_create(&timer_args, &s_retry_timer);
    if (err != ESP_OK) {
        goto cleanup_events;
    }

    /* 配网存进NVS的凭据优先于编译期凭据(secrets.h只作首次兜底) */
    char nvs_ssid[33] = {0};
    char nvs_pass[64] = {0};
    if (net_credentials_load(nvs_ssid, sizeof(nvs_ssid), nvs_pass, sizeof(nvs_pass)) == ESP_OK) {
        ssid = nvs_ssid;
        password = nvs_pass;
        ESP_LOGI(TAG, "使用NVS保存的凭据");
    }

    err = wifi_start_sta(ssid, password);
    if (err != ESP_OK) {
        goto cleanup_timer;
    }

    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    return ESP_OK;

cleanup_timer:
    esp_timer_delete(s_retry_timer);
cleanup_events:
    vEventGroupDelete(s_events);
    s_events = NULL;
    return err;
}

net_state_t net_get_state(void)
{
    return s_state;
}

esp_err_t net_manager_register_connected_cb(net_connected_cb_t cb)
{
    s_connected_cb = cb;
    return ESP_OK;
}

esp_err_t net_wait_connected(int timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_INVALID_STATE, TAG, "not inited");

    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT_CONNECTED) ? ESP_OK : ESP_ERR_TIMEOUT;
}

#define NVS_NAMESPACE "net"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"

static esp_err_t net_credentials_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err; /* namespace不存在(从未配网)也在这里返回 */
    }
    err = nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, NVS_KEY_PASS, pass, &pass_len);
    }
    nvs_close(handle);
    return err;
}

esp_err_t net_credentials_save(const char *ssid, const char *pass)
{
    ESP_RETURN_ON_FALSE(ssid && pass, ESP_ERR_INVALID_ARG, TAG, "bad arg");

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_PASS, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t net_apply_credentials(const char *ssid, const char *pass)
{
    ESP_RETURN_ON_FALSE(ssid && pass, ESP_ERR_INVALID_ARG, TAG, "bad arg");
    ESP_RETURN_ON_FALSE(strlen(ssid) < 33 && strlen(pass) < 64, ESP_ERR_INVALID_SIZE, TAG, "too long");

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);

    /* 清零重试计数并先设新凭据：随后DISCONNECTED事件回调会用新凭据立即重连 */
    esp_timer_stop(s_retry_timer);
    s_retry = 0;
    s_state = NET_CONNECTING;
    xEventGroupClearBits(s_events, BIT_CONNECTED);
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "set config");

    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_ERR_WIFI_NOT_CONNECT) {
        /* 本来断开就不会有DISCONNECTED事件，自己发起连接 */
        ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "connect");
    } else if (err != ESP_OK) {
        ESP_RETURN_ON_ERROR(err, TAG, "disconnect");
    }
    return ESP_OK;
}

esp_err_t net_get_current_ssid(char *buf, size_t buf_len)
{
    ESP_RETURN_ON_FALSE(buf && buf_len, ESP_ERR_INVALID_ARG, TAG, "bad arg");

    wifi_config_t cfg;
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &cfg), TAG, "get config");
    if (strlen((char *)cfg.sta.ssid) >= buf_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    strcpy(buf, (char *)cfg.sta.ssid);
    return ESP_OK;
}

/* 配网切换前的旧配置快照，供配网失败后一键恢复，避免输错密码导致长期断网 */
static wifi_config_t s_prev_config;
static bool s_prev_valid = false;

void net_credentials_snapshot(void)
{
    if (esp_wifi_get_config(WIFI_IF_STA, &s_prev_config) == ESP_OK) {
        s_prev_valid = true;
    }
}

esp_err_t net_credentials_restore(void)
{
    if (!s_prev_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &s_prev_config), TAG, "restore config");
    s_prev_valid = false;

    esp_timer_stop(s_retry_timer);
    s_retry = 0;
    s_state = NET_CONNECTING;
    xEventGroupClearBits(s_events, BIT_CONNECTED);
    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "reconnect");
    } else if (err != ESP_OK) {
        ESP_RETURN_ON_ERROR(err, TAG, "disconnect");
    }
    return ESP_OK;
}
