#include "app_runtime.h"

#include <inttypes.h>

#include "battery.h"
#include "board_rlcd42.h"
#include "claude_quota_client.h"
#include "display_st7305.h"
#include "env_logger.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "input_manager.h"
#include "net_manager.h"
#include "rtc_pcf85063.h"
#include "sensor_shtc3.h"
#include "storage_sd.h"
#include "sys_state.h"
#include "time_service.h"
#include "ui_manager.h"
#include "ui_pages.h"
#include "weather_client.h"

static const char *TAG = "app_runtime";

/* 可选外设/服务失败不阻断启动，只降级+记日志(规范第5节: 驱动/服务层不能ESP_ERROR_CHECK) */
static void log_if_failed(esp_err_t err, const char *what)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s失败: %s", what, esp_err_to_name(err));
    }
}

/* WiFi首次连接成功时触发(不论是否在15s等待窗口内)，保证延迟联网也能自动校时 */
static void on_wifi_connected(void)
{
    log_if_failed(time_service_start_sntp(), "SNTP启动");
}

/* 网络与校时异步进行: 没网也要能显示时间(RTC)和温湿度，所以不阻塞UI启动 */
static void start_network(const app_runtime_config_t *cfg)
{
    /* 须在net_init()前注册，否则若GOT_IP在注册前已触发会漏接 */
    net_manager_register_connected_cb(on_wifi_connected);

    esp_err_t err = net_init(cfg->wifi_ssid, cfg->wifi_password);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi初始化失败: %s，离线运行", esp_err_to_name(err));
        return;
    }

    /* 先等WiFi真连上(最多15s)再启动轮询任务，避免首次请求抢在网络就绪前必然
     * 失败；超时也照常启动让任务自行重试。SNTP不受此限制，由on_wifi_connected触发 */
    bool connected = (net_wait_connected(15000) == ESP_OK);
    if (!connected) {
        ESP_LOGW(TAG, "WiFi连接超时，后台继续重试");
    }

    log_if_failed(claude_quota_client_init(cfg->bridge_mdns_host, cfg->bridge_port), "Claude用量客户端启动");
    log_if_failed(weather_client_init(cfg->bridge_mdns_host, cfg->bridge_port), "天气客户端启动");
}

esp_err_t app_runtime_start(const app_runtime_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");

    /* 硬件初始化失败无法继续，此处允许 ESP_ERROR_CHECK (规范第5节) */
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(st7305_init());
    ESP_ERROR_CHECK(rtc_pcf85063_init());
    ESP_ERROR_CHECK(shtc3_init());
    ESP_ERROR_CHECK(battery_init());
    ESP_ERROR_CHECK(sys_state_init());

    /* 以下均为可选外设/后台服务，失败降级不阻断启动 */
    log_if_failed(storage_sd_init(), "SD卡挂载");
    log_if_failed(time_service_init(cfg->timezone), "时间服务初始化");
    log_if_failed(env_logger_init(), "环境记录任务启动");

    ESP_ERROR_CHECK(input_manager_init());
    ESP_ERROR_CHECK(ui_manager_init());
    ESP_ERROR_CHECK(ui_manager_register_page(page_home_get()));
    ESP_ERROR_CHECK(ui_manager_register_page(page_env_monitor_get()));
    ESP_ERROR_CHECK(ui_manager_register_page(page_quota_get()));
    ESP_ERROR_CHECK(ui_manager_register_page(page_weather_get()));
    ESP_ERROR_CHECK(ui_manager_start());

    start_network(cfg);
    ESP_LOGI(TAG, "启动完成 | 堆剩余 %" PRIu32 "B(历史最低%" PRIu32 "B) | PSRAM剩余%" PRIu32 "B(历史最低%" PRIu32 "B)",
             (uint32_t)esp_get_free_heap_size(), (uint32_t)esp_get_minimum_free_heap_size(),
             (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}
