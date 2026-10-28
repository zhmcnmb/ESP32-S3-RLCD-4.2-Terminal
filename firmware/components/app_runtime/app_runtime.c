#include "app_runtime.h"

#include <inttypes.h>

#include "agent_runtime.h"
#include "asr_client.h"
#include "audio_pipeline.h"
#include "battery.h"
#include "board_rlcd42.h"
#include "codex_usage_client.h"
#include "display_st7305.h"
#include "env_logger.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "input_manager.h"
#include "llm_provider.h"
#include "web_client.h"
#include "net_manager.h"
#include "rtc_pcf85063.h"
#include "sensor_shtc3.h"
#include "scheduler.h"
#include "storage_sd.h"
#include "sys_state.h"
#include "text_console.h"
#include "time_service.h"
#include "tts_client.h"
#include "ui_manager.h"
#include "ui_pages.h"
#include "voice_assistant.h"
#include "weather_client.h"
#include "wifi_setup.h"

static const char *TAG = "app_runtime";
/* ponytail: 整条语音链路停用；恢复时改 1。 */
#define APP_VOICE_ENABLED 0


/* 可选外设/服务失败不阻断启动，只降级+记日志(规范第5节: 驱动/服务层不能ESP_ERROR_CHECK) */
static void log_if_failed(esp_err_t err, const char *what)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s失败: %s", what, esp_err_to_name(err));
    }
}

/* B1: 每300s打印一次资源水位(启动日志同款四指标)，供长时间运行的内存趋势观察；
 * 事件回调只做ESP_LOGI，无阻塞，符合FreeRTOS事件回调约束(见规范第7节) */
#define RESOURCE_LOG_PERIOD_US (300LL * 1000000LL)

static void resource_log_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "资源水位 | 堆剩余 %" PRIu32 "B(历史最低%" PRIu32 "B) | PSRAM剩余%" PRIu32 "B(历史最低%" PRIu32 "B)",
             (uint32_t)esp_get_free_heap_size(), (uint32_t)esp_get_minimum_free_heap_size(),
             (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
}

/* SNTP启动定时器回调：在esp_timer任务上下文执行，不在WiFi/IP事件回调中阻塞 */
static void sntp_start_cb(void *arg)
{
    (void)arg;
    log_if_failed(time_service_start_sntp(), "SNTP启动");
}

/* WiFi首次连接成功时触发(不论是否在15s等待窗口内)，保证延迟联网也能自动校时。
 * 用一次性esp_timer把SNTP启动延迟到定时器任务上下文，避免在IP事件回调中阻塞
 * (规范第7节：事件回调禁止网络访问和等待锁) */
static void on_wifi_connected(void)
{
    static esp_timer_handle_t sntp_timer = NULL;
    if (!sntp_timer) {
        const esp_timer_create_args_t args = {
            .callback = sntp_start_cb,
            .name = "sntp_start",
        };
        if (esp_timer_create(&args, &sntp_timer) != ESP_OK) {
            ESP_LOGE(TAG, "创建SNTP定时器失败，直接启动");
            log_if_failed(time_service_start_sntp(), "SNTP启动");
            return;
        }
    }
    esp_err_t err = esp_timer_start_once(sntp_timer, 0); /* 立即触发，但在esp_timer任务上下文执行 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP定时器启动失败: %s，直接启动", esp_err_to_name(err));
        log_if_failed(time_service_start_sntp(), "SNTP启动");
    }
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
    log_if_failed(wifi_setup_init(), "WiFi配网组件启动");

    /* 先等WiFi真连上(最多15s)再启动轮询任务，避免首次请求抢在网络就绪前必然
     * 失败；超时也照常启动让任务自行重试。SNTP不受此限制，由on_wifi_connected触发 */
    bool connected = (net_wait_connected(15000) == ESP_OK);
    if (!connected) {
        ESP_LOGW(TAG, "WiFi连接超时，后台继续重试");
    }

    log_if_failed(weather_client_init(cfg->qweather_api_key, cfg->qweather_api_host, cfg->qweather_location),
                  "天气客户端启动");
    log_if_failed(codex_usage_client_init(cfg->codex_usage_base_url, cfg->codex_usage_api_key),
                  "Codex usage client");

    /* P5.0 云API客户端：ASR/TTS固定走SiliconFlow；LLM可选DeepSeek直连，
     * 否则使用配置给定的SiliconFlow模型，不把模型名硬编码在运行时。 */
#if APP_VOICE_ENABLED
    log_if_failed(asr_client_init(cfg->siliconflow_api_key), "ASR客户端启动");
    log_if_failed(tts_client_init(cfg->siliconflow_api_key), "TTS客户端启动");
#endif
    /* LLM与免Key检索仅服务agent，agent下线后无消费者，一并注释 */
    //if (cfg->use_deepseek_llm) {
    //    log_if_failed(llm_provider_init(cfg->deepseek_api_key, "deepseek-chat",
    //                                    cfg->deepseek_llm_url),
    //                  "LLM客户端启动(DeepSeek)");
    //} else {
    //    log_if_failed(llm_provider_init(cfg->siliconflow_api_key,
    //                                    cfg->siliconflow_llm_model, cfg->siliconflow_llm_url),
    //                  "LLM客户端启动(SiliconFlow)");
    //}
    ///* 免Key检索(Bing搜索+白名单URL直连抓取正文) */
    //log_if_failed(web_client_init(), "网页客户端启动");
}
esp_err_t app_runtime_start(const app_runtime_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");

    /* 板级硬件失败不可恢复：返回给 app_main，由其唯一决定是否 abort */
    ESP_RETURN_ON_ERROR(board_init(), TAG, "board_init");
    ESP_RETURN_ON_ERROR(st7305_init(), TAG, "st7305_init");
    ESP_RETURN_ON_ERROR(rtc_pcf85063_init(), TAG, "rtc_pcf85063_init");
    ESP_RETURN_ON_ERROR(shtc3_init(), TAG, "shtc3_init");
    ESP_RETURN_ON_ERROR(battery_init(), TAG, "battery_init");
    ESP_RETURN_ON_ERROR(sys_state_init(), TAG, "sys_state_init");

    /* 省电：DFS频段40~80MHz(上限与CPU配置一致)；外设驱动(IDF各总线/WiFi)
     * 内部持pm lock保时序，SDK端自动管理。
     * light_sleep_enable保持false：auto light sleep会挂起USB-Serial-JTAG，
     * 串口日志和烧录通道一并失效(COM3拒绝访问)，需要时改为true */
    const esp_pm_config_t pm_cfg = {
        .max_freq_mhz = 80,
        .min_freq_mhz = 40,
        .light_sleep_enable = false,
    };
    ESP_RETURN_ON_ERROR(esp_pm_configure(&pm_cfg), TAG, "esp_pm_configure");

    /* 以下均为可选外设/后台服务，失败降级不阻断启动 */
    log_if_failed(storage_sd_init(), "SD卡挂载");
    log_if_failed(time_service_init(cfg->timezone), "时间服务初始化");
    /* 提醒的创建入口是agent工具、到点播报唯一消费者是已关的voice_assistant，
     * UI无提醒显示，整条链路无消费者，一并注释 */
    //log_if_failed(scheduler_init(), "提醒调度器启动");
    log_if_failed(env_logger_init(), "环境记录任务启动");
#if APP_VOICE_ENABLED
    log_if_failed(audio_pipeline_init(), "音频链路初始化(P5.1)");
#endif

    ESP_RETURN_ON_ERROR(input_manager_init(), TAG, "input_manager_init");
    ESP_RETURN_ON_ERROR(ui_manager_init(), TAG, "ui_manager_init");
    ESP_RETURN_ON_ERROR(ui_manager_register_page(page_codex_usage_get()), TAG, "register codex usage");
    /* agent界面先注释下线：语音已关(APP_VOICE_ENABLED=0)，Jarvis页无消费者；
     * agent本体(agent_runtime+串口注入)仍在运行，恢复显示时取消注释即可 */
    //ESP_RETURN_ON_ERROR(ui_manager_register_page(page_jarvis_get()), TAG, "register jarvis");
    ESP_RETURN_ON_ERROR(ui_manager_register_page(page_home_get()), TAG, "register home");
    ESP_RETURN_ON_ERROR(ui_manager_register_page(page_weather_get()), TAG, "register weather");
    ESP_RETURN_ON_ERROR(ui_manager_register_page(page_env_monitor_get()), TAG, "register env");
    ESP_RETURN_ON_ERROR(ui_manager_register_page(page_wifi_get()), TAG, "register wifi");
    ESP_RETURN_ON_ERROR(ui_manager_start(), TAG, "ui_manager_start");

    start_network(cfg);
    /* agent本体先注释下线：语音已关后串口文本注入唯一消费者(voice_assistant文本轮)
     * 也不运行，整条agent链路无入口；恢复agent时连同一并取消注释 */
    //log_if_failed(agent_runtime_init(), "个人智能体运行时启动");
#if APP_VOICE_ENABLED
    /* 云客户端和个人智能体就绪后再启动语音闭环；音频未就绪则只降级 */
    log_if_failed(voice_assistant_start(), "语音助手启动");
#else
    ESP_LOGI(TAG, "语音交互已关闭");
#endif
    //log_if_failed(text_console_init(), "串口文本控制台启动");
    ESP_LOGI(TAG, "启动完成 | 堆剩余 %" PRIu32 "B(历史最低%" PRIu32 "B) | PSRAM剩余%" PRIu32 "B(历史最低%" PRIu32 "B)",
             (uint32_t)esp_get_free_heap_size(), (uint32_t)esp_get_minimum_free_heap_size(),
             (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));

    esp_timer_handle_t resource_log_timer = NULL;
    const esp_timer_create_args_t resource_log_args = {
        .callback = resource_log_cb,
        .name = "res_log",
    };
    esp_err_t timer_err = esp_timer_create(&resource_log_args, &resource_log_timer);
    if (timer_err == ESP_OK) {
        timer_err = esp_timer_start_periodic(resource_log_timer, RESOURCE_LOG_PERIOD_US);
    }
    if (timer_err != ESP_OK) {
        if (resource_log_timer) {
            esp_err_t delete_err = esp_timer_delete(resource_log_timer);
            if (delete_err != ESP_OK) {
                ESP_LOGW(TAG, "资源水位定时器清理失败: %s", esp_err_to_name(delete_err));
            }
        }
        ESP_LOGW(TAG, "资源水位定时器启动失败: %s", esp_err_to_name(timer_err));
    }
    return ESP_OK;
}
