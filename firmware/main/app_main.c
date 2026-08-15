#include <inttypes.h>

#include "app_runtime.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "secrets.h"

/* 启动编排(docs/coding-standards.md 第3节): 只做芯片诊断+调用app_runtime，
 * 具体初始化顺序/页面注册/联网/语音全部下沉到 app_runtime */

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

void app_main(void)
{
    log_chip_info();

    const app_runtime_config_t cfg = {
        .wifi_ssid = WIFI_SSID,
        .wifi_password = WIFI_PASSWORD,
        .timezone = TIMEZONE,
        .qweather_api_key = QWEATHER_API_KEY,
        .qweather_api_host = QWEATHER_API_HOST,
        .qweather_location = QWEATHER_LOCATION,
        .siliconflow_api_key = SILICONFLOW_API_KEY,
        .siliconflow_llm_model = "deepseek-ai/DeepSeek-V4-Flash",
        .siliconflow_llm_url = "https://api.siliconflow.cn/v1/chat/completions",
        .deepseek_api_key = DEEPSEEK_API_KEY,
        .deepseek_llm_url = "https://api.deepseek.com/v1/chat/completions",
        .use_deepseek_llm = false, /* 对话继续走SiliconFlow，不切到DeepSeek直连 */
    };
    ESP_ERROR_CHECK(app_runtime_start(&cfg));
}
