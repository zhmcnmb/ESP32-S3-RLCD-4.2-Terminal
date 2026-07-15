/*
 * ESP-SR AFE(音频前端) + WakeNet(唤醒词) + VAD 实现
 *
 * 唤醒词模型: wn9_hilexin (Hi,乐鑫)
 * 触发方式: 说出"Hi,乐鑫"或"嗨，乐鑫"后唤醒
 * 后续更换自定义唤醒词需在 menuconfig 中重新选择模型并重新烧录
 *
 * 模型加载: 通过 menuconfig 选择 CONFIG_SR_WN_WN9_HILEXIN=y，
 *           idf.py flash 时自动烧录到 "model" 分区
 *
 * AFE配置来源: esp-sr 官方 AFE 框架示例
 *   https://github.com/espressif/esp-skainet/blob/master/examples/wake_word_detection/afe/main/main.c
 */

#include <stdlib.h>
#include <string.h>

#include "wakeword.h"
#include "audio_pipeline.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_config.h"
#include "model_path.h"
#include "esp_wn_iface.h"
#include "esp_afe_sr_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wakeword";

/* ---------- 常量 ---------- */

/* 语音段最大录音时长: 15秒(与 asr_client.c 的 MAX_WAV_SIZE 对齐) */
#define UTTERANCE_MAX_SECONDS   15
#define UTTERANCE_MAX_SAMPLES   (UTTERANCE_MAX_SECONDS * AUDIO_PIPELINE_SAMPLE_RATE)

/* ---------- 状态 ---------- */

typedef enum {
    STATE_IDLE,         /* 等待唤醒词 */
    STATE_LISTENING,    /* 唤醒后监听语音 */
} detect_state_t;

static struct {
    wakeword_event_cb_t cb;
    void *ctx;
    const esp_afe_sr_iface_t *afe_handle;
    esp_afe_sr_data_t *afe_data;
    TaskHandle_t task;
    volatile bool running;
    detect_state_t state;
    bool vad_was_active;

    /* 语音段缓冲: SPEECH_START 到 SPEECH_END 之间 AFE 输出的 PCM */
    int16_t *utterance_buf;     /* PSRAM 分配，最大 UTTERANCE_MAX_SAMPLES 个样本 */
    size_t utterance_count;     /* 本轮已累积样本数 */
    size_t utterance_cap;       /* 缓冲容量(样本数) */
} s_ctx;

/* ---------- 内部辅助函数 ---------- */

/**
 * 延迟分配语音段缓冲(首次 SPEECH_START 时分配，减少空闲时内存占用)
 */
static esp_err_t ensure_utterance_buf(void)
{
    if (s_ctx.utterance_buf) {
        return ESP_OK;
    }
    size_t bytes = UTTERANCE_MAX_SAMPLES * sizeof(int16_t);  /* 480KB */
    s_ctx.utterance_buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!s_ctx.utterance_buf) {
        ESP_LOGE(TAG, "分配语音段缓冲失败(%u KB)", (unsigned)(bytes / 1024));
        return ESP_ERR_NO_MEM;
    }
    s_ctx.utterance_cap = UTTERANCE_MAX_SAMPLES;
    ESP_LOGI(TAG, "语音段缓冲已分配: %u 样本 / %u KB PSRAM",
             (unsigned)UTTERANCE_MAX_SAMPLES, (unsigned)(bytes / 1024));
    return ESP_OK;
}

/**
 * 将 AFE fetch 输出的 PCM 追加到语音段缓冲(溢出截断)
 */
static void append_utterance(const int16_t *data, size_t count)
{
    if (!s_ctx.utterance_buf || count == 0) {
        return;
    }
    size_t space = s_ctx.utterance_cap - s_ctx.utterance_count;
    size_t copy = (count < space) ? count : space;
    if (copy < count) {
        ESP_LOGW(TAG, "语音段缓冲已满(截断 %u 样本)", (unsigned)(count - copy));
    }
    memcpy(&s_ctx.utterance_buf[s_ctx.utterance_count], data, copy * sizeof(int16_t));
    s_ctx.utterance_count += copy;
}

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
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG, "模型分区加载失败，确认 menuconfig 已选唤醒词并重新烧录");
        return ESP_ERR_NOT_FOUND;
    }
    for (int i = 0; i < models->num; i++) {
        if (strstr(models->model_name[i], ESP_WN_PREFIX) != NULL) {
            ESP_LOGI(TAG, "WakeNet模型: %s", models->model_name[i]);
        }
    }

    /* 2. 初始化AFE配置
     * input_format="M": 单麦克风单通道。audio_pipeline的ES7210配置是
     * STD I2S mono(mic_selected=MIC1)，不是TDM多通道，没有AEC参考回采，
     * 也没有额外未用通道——"MNR"(3通道)会导致feed_nch=3但实际数据源只有
     * 1个真实声道，造成AFE把mono数据错误按3通道交错解读(真机上表现为
     * feed_chunk=512 ch=3，实际音频被压缩到1/3时长且声道错位) */
    afe_config_t *afe_config = afe_config_init("M", models,
                                                AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!afe_config) {
        ESP_LOGE(TAG, "AFE配置初始化失败");
        return ESP_FAIL;
    }
    /* ESP32-S3 WiFi/lwIP协议栈固定跑CPU0，AFE内部se task(afe_perferred_core)
     * 和本组件的feed/fetch task若都挤CPU0，真机会把IDLE0饿死触发watchdog
     * (task_wdt报"CPU 0: wakeword")——AFE内部task和外部task统一挪到CPU1 */
    afe_config->afe_perferred_core = 1;
    if (afe_config->wakenet_model_name) {
        ESP_LOGI(TAG, "唤醒词: %s", afe_config->wakenet_model_name);
    }

    /* 3. 创建AFE实例 */
    s_ctx.afe_handle = esp_afe_handle_from_config(afe_config);
    if (!s_ctx.afe_handle) {
        ESP_LOGE(TAG, "获取AFE handle失败");
        afe_config_free(afe_config);
        return ESP_FAIL;
    }
    s_ctx.afe_data = s_ctx.afe_handle->create_from_config(afe_config);
    if (!s_ctx.afe_data) {
        ESP_LOGE(TAG, "创建AFE实例失败");
        afe_config_free(afe_config);
        return ESP_FAIL;
    }
    afe_config_free(afe_config);

    ESP_LOGI(TAG, "AFE初始化成功");
    return ESP_OK;
}

/**
 * 唤醒词检测任务(合并feed+fetch为单任务)
 *
 * 栈大小依据: esp-skainet 官方示例中 feed_task=8KB, detect_task=4KB，
 * 本项目合并后取较大值8KB。优先级5与ui_task同级(语音交互优先于UI刷新)。
 * 绑定核心0，避免与audio_pipeline的I2S DMA中断(核心1)争抢。
 * 来源: esp-skainet/examples/wake_word_detection/afe/main/main.c
 */
static void wakeword_task(void *arg)
{
    int feed_chunksize = s_ctx.afe_handle->get_feed_chunksize(s_ctx.afe_data);
    int feed_nch = s_ctx.afe_handle->get_feed_channel_num(s_ctx.afe_data);
    int fetch_chunksize = s_ctx.afe_handle->get_fetch_chunksize(s_ctx.afe_data);

    /* feed 缓冲: 通道交错格式，每帧包含所有通道样本 */
    int16_t *feed_buff = heap_caps_malloc(
        feed_chunksize * feed_nch * sizeof(int16_t),
        MALLOC_CAP_SPIRAM);
    if (!feed_buff) {
        ESP_LOGE(TAG, "分配feed缓冲失败");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "检测任务启动: feed_chunk=%d ch=%d fetch_chunk=%d",
             feed_chunksize, feed_nch, fetch_chunksize);

    s_ctx.state = STATE_IDLE;
    s_ctx.vad_was_active = false;

    while (s_ctx.running) {
        /* 1. 拉PCM */
        size_t need = (size_t)(feed_chunksize * feed_nch);
        size_t got = audio_pipeline_read(feed_buff, need, 1000);
        if (got == 0) {
            if (!audio_pipeline_is_ready()) {
                ESP_LOGW(TAG, "音频管线未就绪");
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            continue;
        }
        if (got < need) {
            memset(&feed_buff[got], 0, (need - got) * sizeof(int16_t));
        }

        /* 2. 喂给AFE */
        s_ctx.afe_handle->feed(s_ctx.afe_data, feed_buff);

        /* 3. 获取检测结果 */
        afe_fetch_result_t *res = s_ctx.afe_handle->fetch_with_delay(
            s_ctx.afe_data, 100 / portTICK_PERIOD_MS);
        if (!res || res->ret_value == ESP_FAIL) {
            ESP_LOGW(TAG, "fetch失败");
            continue;
        }

        /* 4. 唤醒词命中 */
        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "唤醒! model=%d word=%d",
                     res->wakenet_model_index, res->wake_word_index);
            if (s_ctx.cb) {
                s_ctx.cb(WAKEWORD_EVENT_WAKE, s_ctx.ctx);
            }
            s_ctx.state = STATE_LISTENING;
            s_ctx.vad_was_active = false;
        }

        /* 5. 监听状态: VAD + 语音段缓冲 */
        if (s_ctx.state == STATE_LISTENING) {
            bool vad_active = (res->vad_state == VAD_SPEECH);

            /* 语音开始 */
            if (vad_active && !s_ctx.vad_was_active) {
                ESP_LOGI(TAG, "语音开始");
                if (ensure_utterance_buf() == ESP_OK) {
                    s_ctx.utterance_count = 0;   /* 新一轮清空 */
                }
                if (s_ctx.cb) {
                    s_ctx.cb(WAKEWORD_EVENT_SPEECH_START, s_ctx.ctx);
                }
            }

            /* 追加 AFE 输出到语音段缓冲 */
            if (s_ctx.utterance_buf && res->data && res->data_size > 0) {
                size_t samples = res->data_size / sizeof(int16_t);
                append_utterance(res->data, samples);
            }

            /* 语音结束 */
            if (!vad_active && s_ctx.vad_was_active) {
                ESP_LOGI(TAG, "语音结束 (共%u样本)", (unsigned)s_ctx.utterance_count);
                if (s_ctx.cb) {
                    s_ctx.cb(WAKEWORD_EVENT_SPEECH_END, s_ctx.ctx);
                }
                s_ctx.state = STATE_IDLE;
            }

            s_ctx.vad_was_active = vad_active;
        }
    }

    /* 清理任务内部分配 */
    if (feed_buff) {
        heap_caps_free(feed_buff);
    }
    ESP_LOGI(TAG, "检测任务退出");
    vTaskDelete(NULL);
}

/* ========== 公开接口 ========== */

esp_err_t wakeword_init(wakeword_event_cb_t cb, void *ctx)
{
    if (!cb) {
        ESP_LOGE(TAG, "回调函数不能为空");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.afe_data) {
        ESP_LOGW(TAG, "已初始化");
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.cb = cb;
    s_ctx.ctx = ctx;
    s_ctx.running = false;
    s_ctx.task = NULL;
    s_ctx.state = STATE_IDLE;
    s_ctx.utterance_buf = NULL;
    s_ctx.utterance_count = 0;
    s_ctx.utterance_cap = 0;

    esp_err_t ret = init_afe();
    if (ret != ESP_OK) {
        memset(&s_ctx, 0, sizeof(s_ctx));
        return ret;
    }

    ESP_LOGI(TAG, "初始化完成");
    return ESP_OK;
}

size_t wakeword_get_utterance(int16_t *out, size_t max_count)
{
    if (!out || max_count == 0 || !s_ctx.utterance_buf ||
        s_ctx.utterance_count == 0) {
        return 0;
    }
    size_t copy = (max_count < s_ctx.utterance_count)
                  ? max_count : s_ctx.utterance_count;
    memcpy(out, s_ctx.utterance_buf, copy * sizeof(int16_t));
    return copy;
}

esp_err_t wakeword_start(void)
{
    if (!s_ctx.afe_data) {
        ESP_LOGE(TAG, "未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.running) {
        ESP_LOGW(TAG, "已在运行");
        return ESP_ERR_INVALID_STATE;
    }
    if (!audio_pipeline_is_ready()) {
        ESP_LOGE(TAG, "音频管线未就绪");
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        wakeword_task, "wakeword", 8192, NULL, 5, &s_ctx.task, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建检测任务失败");
        s_ctx.running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "检测任务已启动");
    return ESP_OK;
}

esp_err_t wakeword_stop(void)
{
    if (!s_ctx.running) {
        ESP_LOGW(TAG, "未在运行");
        return ESP_ERR_INVALID_STATE;
    }

    /* 通知任务退出并等待 */
    s_ctx.running = false;
    for (int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (eTaskGetState(s_ctx.task) == eDeleted) {
            break;
        }
    }
    if (eTaskGetState(s_ctx.task) != eDeleted) {
        ESP_LOGE(TAG, "检测任务未能在2秒内退出");
        return ESP_ERR_TIMEOUT;
    }

    s_ctx.task = NULL;
    ESP_LOGI(TAG, "检测任务已停止");
    return ESP_OK;
}

esp_err_t wakeword_deinit(void)
{
    if (!s_ctx.afe_data) {
        ESP_LOGW(TAG, "未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.running) {
        ESP_LOGE(TAG, "请先停止检测任务");
        return ESP_ERR_INVALID_STATE;
    }

    /* 销毁AFE实例 */
    if (s_ctx.afe_handle && s_ctx.afe_data) {
        s_ctx.afe_handle->destroy(s_ctx.afe_data);
    }
    /* 释放语音段缓冲 */
    if (s_ctx.utterance_buf) {
        heap_caps_free(s_ctx.utterance_buf);
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    ESP_LOGI(TAG, "资源已释放");
    return ESP_OK;
}
