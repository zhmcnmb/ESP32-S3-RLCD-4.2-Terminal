#include <inttypes.h>

#include "battery.h"
#include "board_rlcd42.h"
#include "display_st7305.h"
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

/* 网络与校时异步进行: 没网也要能显示时间(RTC)和温湿度，所以不阻塞UI启动 */
static void start_network(void)
{
    esp_err_t err = net_init(WIFI_SSID, WIFI_PASSWORD);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi初始化失败: %s，离线运行", esp_err_to_name(err));
        return;
    }
    if (net_wait_connected(15000) != ESP_OK) {
        ESP_LOGW(TAG, "WiFi连接超时，后台继续重试");
        return;
    }
    if (time_service_start_sntp() != ESP_OK) {
        ESP_LOGW(TAG, "SNTP启动失败");
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

    /* SD卡是可选外设，未插卡不应中止启动 */
    if (storage_sd_init() != ESP_OK) {
        ESP_LOGW(TAG, "SD卡不可用，相关功能降级");
    }

    /* RTC可能掉电过，失败不阻断启动(降级为"时间未校准") */
    if (time_service_init(TIMEZONE) != ESP_OK) {
        ESP_LOGW(TAG, "时间服务初始化失败");
    }

    ESP_ERROR_CHECK(ui_manager_init());
    ESP_ERROR_CHECK(ui_manager_register_page(page_home_get()));
    ESP_ERROR_CHECK(ui_manager_register_page(page_diag_get()));
    ESP_ERROR_CHECK(ui_manager_start());

    start_network();
    ESP_LOGI(TAG, "启动完成");
}
