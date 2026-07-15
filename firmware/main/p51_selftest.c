/*
 * P5.1 本地音频链路自检：启动 audio_pipeline + wakeword，监听唤醒词事件，
 * 唤醒后取回AFE处理过的录音写临时WAV到SD，每5次唤醒跑一轮半双工取消测试。
 * 临时验证代码，P5.2正式语音助手接入后删除。
 *
 * 自动化验证范围：build+flash成功、任务运行无crash、录音WAV生成且格式正确、
 * cancel机制生效、堆/PSRAM多轮稳定。
 * 需要人工确认：实际唤醒识别率、播放爆音（要张口说唤醒词+听喇叭）。
 */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>
#include <math.h>

#include "audio_pipeline.h"
#include "wakeword.h"
#include "storage_sd.h"

#include "p51_selftest.h"

static const char *TAG = "p51_selftest";

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SAMPLE_RATE   16000
#define MAX_UTTERANCE_SAMPLES (15 * SAMPLE_RATE)  /* 15秒上限 */
#define WAV_HEADER_SIZE 44
#define CANCEL_TEST_INTERVAL 5  /* 每5次唤醒跑一轮取消测试 */

/* ---- WAV 写入 ---- */

#pragma pack(push, 1)
typedef struct {
    char     riff[4];        /* "RIFF" */
    uint32_t riff_size;
    char     wave[4];        /* "WAVE" */
    char     fmt[4];         /* "fmt " */
    uint32_t fmt_size;       /* 16 */
    uint16_t audio_format;   /* 1 = PCM */
    uint16_t num_channels;   /* 1 = mono */
    uint32_t sample_rate;    /* 16000 */
    uint32_t byte_rate;      /* 32000 */
    uint16_t block_align;    /* 2 */
    uint16_t bits_per_sample;/* 16 */
    char     data[4];        /* "data" */
    uint32_t data_size;
} wav_header_t;
#pragma pack(pop)

static void build_wav_header(wav_header_t *h, uint32_t pcm_bytes)
{
    memcpy(h->riff, "RIFF", 4);
    h->riff_size      = 36 + pcm_bytes;
    memcpy(h->wave, "WAVE", 4);
    memcpy(h->fmt, "fmt ", 4);
    h->fmt_size       = 16;
    h->audio_format   = 1;
    h->num_channels   = 1;
    h->sample_rate    = SAMPLE_RATE;
    h->byte_rate      = SAMPLE_RATE * 2;
    h->block_align    = 2;
    h->bits_per_sample = 16;
    memcpy(h->data, "data", 4);
    h->data_size      = pcm_bytes;
}

/* 把 utterance PCM 写成 /sdcard/p51_test.wav。返回ESP_OK或错误码 */
static esp_err_t write_wav_to_sd(const int16_t *pcm, size_t sample_count)
{
    if (!storage_sd_is_mounted()) {
        ESP_LOGW(TAG, "SD未挂载，跳过WAV写入");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t pcm_bytes = (uint32_t)sample_count * 2;
    uint32_t total = WAV_HEADER_SIZE + pcm_bytes;

    /* PSRAM缓冲：storage_sd_atomic_replace 要求data在RAM(非flash映射)，
     * PSRAM属于DMA可达的RAM，满足要求(P5.0踩坑记录) */
    uint8_t *buf = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "WAV缓冲分配失败 %uB", total);
        return ESP_ERR_NO_MEM;
    }

    build_wav_header((wav_header_t *)buf, pcm_bytes);
    memcpy(buf + WAV_HEADER_SIZE, pcm, pcm_bytes);

    esp_err_t err = storage_sd_atomic_replace("p51_test.wav", buf, total);
    free(buf);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WAV写入SD失败: %s", esp_err_to_name(err));
    }
    return err;
}

/* ---- 半双工取消测试 ---- */

static void cancel_timer_cb(void *arg)
{
    (void)arg;
    audio_pipeline_cancel();
}

/* 停wakeword→合成tone→write阻塞→定时器cancel→验证返回NOT_FINISHED→清flag→重启wakeword */
static void run_cancel_test(void)
{
    ESP_LOGI(TAG, "=== 半双工取消测试 ===");

    /* wakeword内部task持续read mic，取消测试期间先停掉避免cancel flag干扰read */
    wakeword_stop();

    /* 合成2秒1kHz正弦波(PSRAM)，amplitude适中避免爆音 */
    size_t tone_samples = 2 * SAMPLE_RATE;
    int16_t *tone = heap_caps_malloc(tone_samples * 2, MALLOC_CAP_SPIRAM);
    if (!tone) {
        ESP_LOGE(TAG, "tone缓冲分配失败");
        wakeword_start();
        return;
    }
    for (size_t i = 0; i < tone_samples; i++) {
        tone[i] = (int16_t)(8000.0f * sinf(2.0f * M_PI * 1000.0f * i / SAMPLE_RATE));
    }

    /* 500ms后触发cancel */
    esp_timer_handle_t timer;
    esp_timer_create_args_t timer_cfg = {
        .callback = cancel_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "p51_cancel",
    };
    esp_timer_create(&timer_cfg, &timer);

    audio_pipeline_clear_cancel();
    esp_timer_start_once(timer, 500 * 1000); /* 500ms */

    int64_t t0 = esp_timer_get_time();
    esp_err_t wr = audio_pipeline_write(tone, tone_samples, 5000); /* 5s超时上限 */
    int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;

    free(tone);
    esp_timer_delete(timer);

    if (wr == ESP_ERR_NOT_FINISHED) {
        ESP_LOGI(TAG, "取消测试 PASS: write被cancel中断，耗时%lldms (<500ms+缓冲排空)", (long long)elapsed_ms);
    } else if (wr == ESP_OK) {
        ESP_LOGW(TAG, "取消测试 WARN: write正常完成(未被cancel中断)，耗时%lldms——可能缓冲全部在500ms内排空了", (long long)elapsed_ms);
    } else {
        ESP_LOGE(TAG, "取消测试 FAIL: write返回%s，耗时%lldms", esp_err_to_name(wr), (long long)elapsed_ms);
    }

    audio_pipeline_clear_cancel();
    wakeword_start();
    ESP_LOGI(TAG, "=== 取消测试结束 ===");
}

/* ---- 事件处理 ---- */

static QueueHandle_t s_evt_queue;
static TaskHandle_t   s_selftest_task;
static int            s_wake_count = 0;
static int64_t        s_speech_start_us = 0;

static void wakeword_cb(wakeword_event_t event, void *ctx)
{
    (void)ctx;
    /* 回调在wakeword task上下文，必须简短非阻塞——只做enqueue */
    xQueueSend(s_evt_queue, &event, 0);
}

static void p51_selftest_task(void *arg)
{
    (void)arg;

    /* wakeword依赖audio_pipeline已init(app_runtime已完成) */
    esp_err_t err = wakeword_init(wakeword_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wakeword_init失败: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    err = wakeword_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wakeword_start失败: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "P5.1自检就绪 | 堆%dB PSRAM%dB | 请说唤醒词测试",
             (int)esp_get_free_heap_size(), (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    wakeword_event_t evt;
    while (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) == pdTRUE) {
        switch (evt) {
        case WAKEWORD_EVENT_WAKE:
            s_wake_count++;
            ESP_LOGI(TAG, "[#%d] 唤醒命中 | 堆%dB(最低%dB) PSRAM%dB(最低%dB)",
                     s_wake_count,
                     (int)esp_get_free_heap_size(), (int)esp_get_minimum_free_heap_size(),
                     (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (int)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
            break;

        case WAKEWORD_EVENT_SPEECH_START:
            s_speech_start_us = esp_timer_get_time();
            ESP_LOGI(TAG, "[#%d] 语音开始", s_wake_count);
            break;

        case WAKEWORD_EVENT_SPEECH_END: {
            int64_t dur_ms = (esp_timer_get_time() - s_speech_start_us) / 1000;
            ESP_LOGI(TAG, "[#%d] 语音结束 时长%lldms", s_wake_count, (long long)dur_ms);

            /* 取回AFE处理过的录音 */
            static int16_t *utt_buf = NULL;
            if (!utt_buf) {
                utt_buf = heap_caps_malloc(MAX_UTTERANCE_SAMPLES * 2, MALLOC_CAP_SPIRAM);
                if (!utt_buf) {
                    ESP_LOGE(TAG, "utterance缓冲分配失败");
                    break;
                }
            }
            size_t n = wakeword_get_utterance(utt_buf, MAX_UTTERANCE_SAMPLES);
            if (n == 0) {
                ESP_LOGW(TAG, "[#%d] 录音为空", s_wake_count);
                break;
            }

            ESP_LOGI(TAG, "[#%d] 录音 %zu样本(%zums) → 写WAV", s_wake_count, n, n * 1000 / SAMPLE_RATE);
            esp_err_t werr = write_wav_to_sd(utt_buf, n);
            if (werr == ESP_OK) {
                ESP_LOGI(TAG, "[#%d] WAV已写入 /sdcard/p51_test.wav (%zuB)",
                         s_wake_count, WAV_HEADER_SIZE + n * 2);
            }

            /* 每5次唤醒跑一轮取消测试 */
            if (s_wake_count % CANCEL_TEST_INTERVAL == 0) {
                run_cancel_test();
            }
            break;
        }

        default:
            break;
        }
    }

    vTaskDelete(NULL);
}

esp_err_t p51_selftest_start(void)
{
    s_evt_queue = xQueueCreate(16, sizeof(wakeword_event_t));
    if (!s_evt_queue) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(p51_selftest_task, "p51_selftest", 4096, NULL, 4, &s_selftest_task);
    if (ok != pdPASS) {
        vQueueDelete(s_evt_queue);
        s_evt_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
