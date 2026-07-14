#include <inttypes.h>

#include "battery.h"
#include "board_rlcd42.h"
#include "claude_quota_client.h"
#include "display_st7305.h"
#include "env_logger.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "net_manager.h"
#include "rtc_pcf85063.h"
#include "secrets.h"
#include "sensor_shtc3.h"
#include "storage_sd.h"
#include "time_service.h"
#include "ui_manager.h"
#include "ui_pages.h"
#include "weather_client.h"

/*
 * 启动编排。按 docs/coding-standards.md 第3节，本文件只做:
 * 初始化 -> 注册页面 -> 启动UI，不写任何业务逻辑(全部下沉到组件)。
 */

static const char *TAG = "app_main";

static void log_chip_info(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "ESP32-S3-RLCD-4.2 | rev %d | flash %" PRIu32 "MB | PSRAM %uMB",
             info.revision, flash_size / (1024 * 1024),
             (unsigned)(esp_psram_get_size() / (1024 * 1024)));
}

/* 可选外设/服务失败不阻断启动，只降级+记日志(规范第4节: 驱动/服务层不能ESP_ERROR_CHECK) */
static void log_if_failed(esp_err_t err, const char *what)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s失败: %s", what, esp_err_to_name(err));
    }
}

/* 网络与校时异步进行: 没网也要能显示时间(RTC)和温湿度，所以不阻塞UI启动 */
static void start_network(void)
{
    esp_err_t err = net_init(WIFI_SSID, WIFI_PASSWORD);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi初始化失败: %s，离线运行", esp_err_to_name(err));
        return;
    }

    /* 先等WiFi真连上(最多15s)再启动这些后台轮询任务，避免它们的首次请求
     * 抢在网络就绪前必然失败——mDNS/DNS都需要实际连通性，不只是netif已init。
     * 超时也照常启动，让各自任务在下个周期自行重试 */
    bool connected = (net_wait_connected(15000) == ESP_OK);
    if (!connected) {
        ESP_LOGW(TAG, "WiFi连接超时，后台继续重试");
    }

    log_if_failed(claude_quota_client_init(BRIDGE_MDNS_HOST, BRIDGE_PORT), "Claude用量客户端启动");
    log_if_failed(weather_client_init(BRIDGE_MDNS_HOST, BRIDGE_PORT), "天气客户端启动");

    if (connected) {
        log_if_failed(time_service_start_sntp(), "SNTP启动");
    }
}

void app_main(void)
{
    log_chip_info();

    /* 硬件初始化失败无法继续，此处允许 ESP_ERROR_CHECK (规范第4节) */
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(st7305_init());
    ESP_ERROR_CHECK(rtc_pcf85063_init());
    ESP_ERROR_CHECK(shtc3_init());
    ESP_ERROR_CHECK(battery_init());

    /* 以下均为可选外设/后台服务，失败降级不阻断启动 */
    log_if_failed(storage_sd_init(), "SD卡挂载");
    log_if_failed(time_service_init(TIMEZONE), "时间服务初始化");
    log_if_failed(env_logger_init(), "环境记录任务启动");

    ESP_ERROR_CHECK(ui_manager_init());
    ESP_ERROR_CHECK(ui_manager_register_page(page_home_get()));
    ESP_ERROR_CHECK(ui_manager_register_page(page_env_monitor_get()));
    ESP_ERROR_CHECK(ui_manager_register_page(page_quota_get()));
    ESP_ERROR_CHECK(ui_manager_register_page(page_weather_get()));
    ESP_ERROR_CHECK(ui_manager_start());

    start_network();
    ESP_LOGI(TAG, "启动完成");
}
