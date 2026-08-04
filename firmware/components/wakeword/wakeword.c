/*
 * ESP-SR AFE(音频前端/VAD) + MultiNet(中文命令词识别) 实现
 *
 * 唤醒方案: mn5q8_cn 单命令词识别，持续监听命令词"贾维斯"(拼音 jia wei si)。
 * 官方WakeNet无"贾维斯"中文发音指纹模型(50+条目表核对过无匹配)，改用
 * MultiNet替代WakeNet作为唤醒网关——MultiNet设计给"唤醒后识别一句命令"用，
 * 非WakeNet那种为常驻监听调优的指纹模型，持续跑存在更高CPU/误唤醒风险，
 * 详见 docs/voice-agent-design.md 第9.4节的决策记录。
 * 触发方式: 说出"贾维斯"后唤醒，唤醒后VAD段落切分沿用原逻辑不变。
 * 模型加载: 通过 menuconfig 选择 CONFIG_SR_MN_CN_MULTINET5_RECOGNITION_QUANT8=y
 *           + CONFIG_CN_SPEECH_COMMAND_ID0="jia wei si"，
 *           idf.py flash 时自动烧录到 "model" 分区
 *
 * AFE配置来源: esp-sr 官方 AFE 框架示例
 *   https://github.com/espressif/esp-skainet/blob/master/examples/wake_word_detection/afe/main/main.c
 * MultiNet持续监听模式来源: esp-skainet 官方中文命令词识别示例
 *   https://github.com/espressif/esp-skainet/blob/master/examples/cn_speech_commands_recognition/main/main.c
 *   (master, 2026-07-16，本组件去掉了官方示例里WakeNet先行门控的wakeup_flag，
 *   改为不经WakeNet门控、直接持续调用multinet->detect())
 */

#include <stdlib.h>
#include <string.h>

#include "wakeword_internal.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_config.h"
#include "model_path.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_process_sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wakeword";

wakeword_context_t g_wakeword_ctx;


/**
 * 初始化AFE配置和实例
 *
 * 来源: esp-skainet 官方 AFE 示例
 *   https://github.com/espressif/esp-skainet/blob/master/examples/wake_word_detection/afe/main/main.c
 *   (master, 2026-07-16)
 */
static esp_err_t init_afe(void)
{
    /* 1. 加载模型分区 */
    g_wakeword_ctx.models = esp_srmodel_init("model");
    if (!g_wakeword_ctx.models) {
        ESP_LOGE(TAG, "模型分区加载失败，确认 menuconfig 已选命令词模型并重新烧录");
        return ESP_ERR_NOT_FOUND;
    }
    for (int i = 0; i < g_wakeword_ctx.models->num; i++) {
        ESP_LOGI(TAG, "已加载模型: %s", g_wakeword_ctx.models->model_name[i]);
    }

    /* 2. 初始化AFE配置
     * input_format="M": 单麦克风单通道。audio_pipeline的ES7210配置是
     * STD I2S mono(mic_selected=MIC1)，不是TDM多通道，没有AEC参考回采，
     * 也没有额外未用通道——"MNR"(3通道)会导致feed_nch=3但实际数据源只有
     * 1个真实声道，造成AFE把mono数据错误按3通道交错解读(真机上表现为
     * feed_chunk=512 ch=3，实际音频被压缩到1/3时长且声道错位) */
    afe_config_t *afe_config = afe_config_init("M", g_wakeword_ctx.models,
                                                AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!afe_config) {
        ESP_LOGE(TAG, "AFE配置初始化失败");
        return ESP_FAIL;
    }
    /* ESP32-S3 WiFi/lwIP协议栈固定跑CPU0，AFE内部se task(afe_perferred_core)
     * 和本组件的feed/fetch task若都挤CPU0，真机会把IDLE0饿死触发watchdog
     * (task_wdt报"CPU 0: wakeword")——AFE内部task和外部task统一挪到CPU1 */
    afe_config->afe_perferred_core = 1;
    /* 唤醒词与问题之间的停顿默认1000ms就判语音结束，会把"贾维斯<停顿>问题"
     * 切成两段、只录到唤醒词。放宽到2000ms，让一次唤醒能录完整句问题 */
    afe_config->vad_min_noise_ms = 2000;
    /* 没有配置WakeNet模型(唤醒改由MultiNet负责)，afe_config->wakenet_model_name
     * 恒为NULL，AFE Pipeline日志里不会再出现WakeNet阶段，这是预期行为 */

    /* 3. 创建AFE实例 */
    g_wakeword_ctx.afe_handle = esp_afe_handle_from_config(afe_config);
    if (!g_wakeword_ctx.afe_handle) {
        ESP_LOGE(TAG, "获取AFE handle失败");
        afe_config_free(afe_config);
        return ESP_FAIL;
    }
    g_wakeword_ctx.afe_data = g_wakeword_ctx.afe_handle->create_from_config(afe_config);
    if (!g_wakeword_ctx.afe_data) {
        ESP_LOGE(TAG, "创建AFE实例失败");
        afe_config_free(afe_config);
        return ESP_FAIL;
    }
    afe_config_free(afe_config);

    ESP_LOGI(TAG, "AFE初始化成功");
    return ESP_OK;
}

/**
 * 初始化MultiNet命令词识别实例(替代WakeNet作为唤醒网关)
 *
 * 依赖 init_afe() 已把模型列表存进内部上下文。命令词从
 * CONFIG_CN_SPEECH_COMMAND_ID0(sdkconfig.defaults里配的"jia wei si")读取，
 * chunk size 必须和 AFE fetch chunk 一致——两者同源于AFE内部固定帧长设计，
 * 不一致说明模型/AFE配置不匹配，返回错误而不是带着错位数据继续跑。
 *
 * 来源: esp-skainet 官方中文命令词识别示例
 *   https://github.com/espressif/esp-skainet/blob/master/examples/cn_speech_commands_recognition/main/main.c
 *   (master, 2026-07-16)
 */
static esp_err_t init_multinet(void)
{
    char *mn_name = esp_srmodel_filter(g_wakeword_ctx.models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (!mn_name) {
        ESP_LOGE(TAG, "未找到中文MultiNet模型，确认 menuconfig 已选 mn5q8_cn");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "MultiNet模型: %s", mn_name);

    g_wakeword_ctx.multinet = esp_mn_handle_from_name(mn_name);
    if (!g_wakeword_ctx.multinet) {
        ESP_LOGE(TAG, "获取MultiNet handle失败");
        return ESP_FAIL;
    }
    g_wakeword_ctx.mn_data = g_wakeword_ctx.multinet->create(
        mn_name, WAKEWORD_MULTINET_DETECT_DURATION_MS);
    if (!g_wakeword_ctx.mn_data) {
        ESP_LOGE(TAG, "创建MultiNet实例失败");
        return ESP_FAIL;
    }

    esp_mn_error_t *err = esp_mn_commands_update_from_sdkconfig(g_wakeword_ctx.multinet, g_wakeword_ctx.mn_data);
    if (err && err->num > 0) {
        ESP_LOGE(TAG, "命令词解析失败: %d 条", err->num);
        g_wakeword_ctx.multinet->destroy(g_wakeword_ctx.mn_data);
        g_wakeword_ctx.mn_data = NULL;
        return ESP_FAIL;
    }

    int mn_chunksize = g_wakeword_ctx.multinet->get_samp_chunksize(g_wakeword_ctx.mn_data);
    int afe_chunksize = g_wakeword_ctx.afe_handle->get_fetch_chunksize(g_wakeword_ctx.afe_data);
    if (mn_chunksize != afe_chunksize) {
        ESP_LOGE(TAG, "MultiNet帧长(%d)与AFE帧长(%d)不一致", mn_chunksize, afe_chunksize);
        g_wakeword_ctx.multinet->destroy(g_wakeword_ctx.mn_data);
        g_wakeword_ctx.mn_data = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    g_wakeword_ctx.multinet->print_active_speech_commands(g_wakeword_ctx.mn_data);
    ESP_LOGI(TAG, "MultiNet初始化成功，持续监听命令词\"贾维斯\"");
    return ESP_OK;
}


/* ========== 公开接口 ========== */

esp_err_t wakeword_init(wakeword_event_cb_t cb, void *ctx)
{
    if (!cb) {
        ESP_LOGE(TAG, "回调函数不能为空");
        return ESP_ERR_INVALID_ARG;
    }
    if (g_wakeword_ctx.afe_data) {
        ESP_LOGW(TAG, "已初始化");
        return ESP_ERR_INVALID_STATE;
    }

    g_wakeword_ctx.cb = cb;
    g_wakeword_ctx.ctx = ctx;
    g_wakeword_ctx.running = false;
    g_wakeword_ctx.task = NULL;
    g_wakeword_ctx.state = STATE_IDLE;
    g_wakeword_ctx.utterance_buf = NULL;
    g_wakeword_ctx.utterance_count = 0;
    g_wakeword_ctx.utterance_cap = 0;

    esp_err_t ret = init_afe();
    if (ret != ESP_OK) {
        if (g_wakeword_ctx.models) {
            esp_srmodel_deinit(g_wakeword_ctx.models);
        }
        memset(&g_wakeword_ctx, 0, sizeof(g_wakeword_ctx));
        return ret;
    }
    ret = init_multinet();
    if (ret != ESP_OK) {
        g_wakeword_ctx.afe_handle->destroy(g_wakeword_ctx.afe_data);
        esp_srmodel_deinit(g_wakeword_ctx.models);
        memset(&g_wakeword_ctx, 0, sizeof(g_wakeword_ctx));
        return ret;
    }

    ESP_LOGI(TAG, "初始化完成");
    return ESP_OK;
}

size_t wakeword_get_utterance(int16_t *out, size_t max_count)
{
    if (!out || max_count == 0 || !g_wakeword_ctx.utterance_buf ||
        g_wakeword_ctx.utterance_count == 0) {
        return 0;
    }
    size_t copy = (max_count < g_wakeword_ctx.utterance_count)
                  ? max_count : g_wakeword_ctx.utterance_count;
    memcpy(out, g_wakeword_ctx.utterance_buf, copy * sizeof(int16_t));
    return copy;
}

esp_err_t wakeword_start(void)
{
    if (!g_wakeword_ctx.afe_data) {
        ESP_LOGE(TAG, "未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    if (g_wakeword_ctx.running || g_wakeword_ctx.task) {
        ESP_LOGW(TAG, "已在运行或旧任务未退出");
        return ESP_ERR_INVALID_STATE;
    }
    if (!audio_pipeline_is_ready()) {
        ESP_LOGE(TAG, "音频管线未就绪");
        return ESP_ERR_INVALID_STATE;
    }

    g_wakeword_ctx.running = true;
    /* 句柄字段直接作出参：内核在任务入就绪队列前写入，任务自清task后
     * 不会再被此处的旧句柄覆盖；失败时内核不写出参，需显式清NULL */
    BaseType_t ret = xTaskCreatePinnedToCore(
        wakeword_task, "wakeword", 8192, NULL, 5,
        (TaskHandle_t *)&g_wakeword_ctx.task, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建检测任务失败");
        g_wakeword_ctx.task = NULL;
        g_wakeword_ctx.running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "检测任务已启动");
    return ESP_OK;
}

esp_err_t wakeword_stop(void)
{
    if (!g_wakeword_ctx.running) {
        ESP_LOGW(TAG, "未在运行");
        return ESP_ERR_INVALID_STATE;
    }

    /* 通知任务退出并等待其自清句柄：任务退出前把task置NULL，避免对已删除
     * 任务的句柄调eTaskGetState(TCB已回收，未定义行为) */
    g_wakeword_ctx.running = false;
    for (int i = 0; i < 20 && g_wakeword_ctx.task; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (g_wakeword_ctx.task) {
        ESP_LOGE(TAG, "检测任务未能在2秒内退出");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "检测任务已停止");
    return ESP_OK;
}

esp_err_t wakeword_trigger(void)
{
    if (!g_wakeword_ctx.afe_data || !g_wakeword_ctx.running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (g_wakeword_ctx.state != STATE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "手动触发录音");
    if (g_wakeword_ctx.cb) {
        g_wakeword_ctx.cb(WAKEWORD_EVENT_WAKE, g_wakeword_ctx.ctx);
    }
    g_wakeword_ctx.state = STATE_LISTENING;
    g_wakeword_ctx.vad_was_active = false;
    return ESP_OK;
}

esp_err_t wakeword_deinit(void)
{
    if (!g_wakeword_ctx.afe_data) {
        ESP_LOGW(TAG, "未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    if (g_wakeword_ctx.running || g_wakeword_ctx.task) {
        ESP_LOGE(TAG, "请先停止检测任务");
        return ESP_ERR_INVALID_STATE;
    }

    /* 销毁MultiNet实例 */
    if (g_wakeword_ctx.multinet && g_wakeword_ctx.mn_data) {
        g_wakeword_ctx.multinet->destroy(g_wakeword_ctx.mn_data);
    }
    /* 销毁AFE实例 */
    if (g_wakeword_ctx.afe_handle && g_wakeword_ctx.afe_data) {
        g_wakeword_ctx.afe_handle->destroy(g_wakeword_ctx.afe_data);
    }
    /* 释放语音段缓冲与模型分区列表 */
    if (g_wakeword_ctx.utterance_buf) {
        heap_caps_free(g_wakeword_ctx.utterance_buf);
    }
    if (g_wakeword_ctx.models) {
        esp_srmodel_deinit(g_wakeword_ctx.models);
    }

    memset(&g_wakeword_ctx, 0, sizeof(g_wakeword_ctx));
    ESP_LOGI(TAG, "资源已释放");
    return ESP_OK;
}
