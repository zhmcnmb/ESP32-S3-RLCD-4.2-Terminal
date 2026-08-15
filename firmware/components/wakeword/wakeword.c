/*
 * ESP-SR AFE(降噪/VAD) 按键录音引擎
 *
 * 触发方式: 无开放式唤醒词——BOOT短按PTT是唯一语音入口(2026-08起MultiNet
 * 常驻唤醒下线)。
 * voice_assistant 消费PTT请求后先 wakeword_start() 拉起本组件任务, 再
 * wakeword_trigger() 注入WAKE事件; 一轮结束 wakeword_stop() 完全停掉AFE
 * 与任务——平时无常驻监听、无推理CPU开销。
 * VAD段落切分: vad_min_noise_ms=2000ms, 一次按键能录完整句问题。
 *
 * AFE配置来源: esp-skainet 官方 AFE 框架示例
 *   https://github.com/espressif/esp-skainet/blob/master/examples/wake_word_detection/afe/main/main.c
 *   (master, 2026-07-16)
 */

#include <stdlib.h>
#include <string.h>

#include "wakeword_internal.h"
#include "esp_log.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_config.h"
#include "model_path.h"
#include "esp_afe_sr_models.h"
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
    /* 1. 加载模型分区(AFE配置需要模型列表, VAD本身不用NN模型) */
    g_wakeword_ctx.models = esp_srmodel_init("model");
    if (!g_wakeword_ctx.models) {
        ESP_LOGE(TAG, "模型分区加载失败，确认已烧录 model 分区");
        return ESP_ERR_NOT_FOUND;
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
    /* 按键与问题之间的停顿默认1000ms就判语音结束，会把"<停顿>问题"
     * 切成两段、只录到残句。放宽到2000ms，让一次按键能录完整句问题 */
    afe_config->vad_min_noise_ms = 2000;

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

    ESP_LOGI(TAG, "初始化完成(按键录音模式)");
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

    /* 状态在创建任务前复位: 任务启动不再触碰state, 否则会把start后紧接着
     * trigger注入的LISTENING冲掉(PTT丢失) */
    g_wakeword_ctx.state = STATE_IDLE;
    g_wakeword_ctx.vad_was_active = false;
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
