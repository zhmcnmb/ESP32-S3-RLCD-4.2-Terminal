#include "net_manager.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
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
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "event group");

    const esp_timer_create_args_t timer_args = {
        .callback = on_retry_timer,
        .name = "net_retry",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_retry_timer), TAG, "retry timer");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL),
        TAG, "reg wifi evt");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL),
        TAG, "reg ip evt");

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "set config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    return ESP_OK;
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
