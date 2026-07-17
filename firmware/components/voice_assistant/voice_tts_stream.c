#include "voice_assistant_internal.h"

#include "audio_pipeline.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "tts_client.h"

static const char *TAG = "voice_tts";

/*
 * 流式播放：网络收多少喂多少给独立播放任务，不等整段合成完再放。
 * 比"缓冲整段再播"少约2-4s感知延迟(合成耗时+落地等待)。
 * CosyVoice2 输出电平偏低(实测峰值4000-6000/32767)，流式场景不知道"全段
 * 峰值"，用固定增益换取低延迟——代价是极端响度的回复可能欠饱和，可接受。
 */
#define TTS_STREAM_BUF_BYTES     (16 * 1024) /* ~0.5s@16kHz mono16bit，吸收网络抖动 */
#define TTS_STREAM_TRIGGER_BYTES 512
#define TTS_FIXED_GAIN_X10       50 /* 5.0x 固定增益，逐样本限幅防溢出 */
#define TTS_PLAYBACK_TASK_STACK  4096
#define TTS_PLAYBACK_WAIT_MS     20000 /* 等播放收尾上限，防调用方卡死 */

static StreamBufferHandle_t s_stream;
static SemaphoreHandle_t s_playback_done;
static volatile bool s_stream_closed;
static volatile bool s_playback_active;
static uint8_t *s_stream_buf;
static StaticStreamBuffer_t s_stream_state;

typedef struct {
    const volatile bool *cancel_flag;
} pcm_ctx_t;

/* 独立播放任务：从流缓冲取数据写 codec，与网络接收(调用方任务)解耦。
 * 首次收到数据才开PA，收完(closed且缓冲已空)后关PA退出，避免频繁开关咔哒声 */
static void playback_task(void *arg)
{
    (void)arg;
    int16_t chunk[TTS_STREAM_TRIGGER_BYTES / sizeof(int16_t)];
    bool pa_on = false;

    for (;;) {
        size_t got = xStreamBufferReceive(s_stream, chunk, sizeof(chunk), pdMS_TO_TICKS(200));
        if (got > 0) {
            if (!pa_on) {
                audio_pipeline_set_output_enable(true);
                pa_on = true;
            }
            esp_err_t err = audio_pipeline_write(chunk, got / sizeof(int16_t));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "音频写入失败: %s", esp_err_to_name(err));
                break;
            }
        } else if (s_stream_closed && xStreamBufferIsEmpty(s_stream)) {
            break;
        }
    }
    if (pa_on) {
        audio_pipeline_set_output_enable(false);
    }
    s_playback_active = false;
    xSemaphoreGive(s_playback_done);
    vTaskDelete(NULL);
}

/* tts_client 分片回调：只做固定增益+推入环形缓冲，播放在独立任务里进行。
 * 缓冲满(播放跟不上)或已取消时返回非OK，中止合成 */
static esp_err_t on_pcm(const int16_t *samples, size_t sample_count, void *ctx)
{
    pcm_ctx_t *pc = ctx;
    if (pc->cancel_flag && *pc->cancel_flag) {
        return ESP_ERR_NOT_FINISHED;
    }
    int16_t gained[128];
    size_t done = 0;
    while (done < sample_count) {
        size_t n = sample_count - done;
        if (n > 128) n = 128;
        for (size_t i = 0; i < n; i++) {
            int32_t v = ((int32_t)samples[done + i] * TTS_FIXED_GAIN_X10) / 10;
            if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
            gained[i] = (int16_t)v;
        }
        size_t want_bytes = n * sizeof(int16_t);
        size_t sent = xStreamBufferSend(s_stream, gained, want_bytes, pdMS_TO_TICKS(3000));
        if (sent < want_bytes) {
            return ESP_ERR_NOT_FINISHED; /* 播放跟不上或超时，中止合成 */
        }
        done += n;
    }
    return ESP_OK;
}

esp_err_t va_tts_stream_init(void)
{
    if (!s_stream) {
        s_stream_buf = heap_caps_malloc(TTS_STREAM_BUF_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_stream_buf) return ESP_ERR_NO_MEM;
        s_stream = xStreamBufferCreateStatic(TTS_STREAM_BUF_BYTES, TTS_STREAM_TRIGGER_BYTES,
                                             s_stream_buf, &s_stream_state);
        if (!s_stream) {
            heap_caps_free(s_stream_buf);
            s_stream_buf = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_playback_done) {
        s_playback_done = xSemaphoreCreateBinary();
        if (!s_playback_done) return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t va_tts_play(const char *text, const volatile bool *cancel_flag, int *out_status)
{
    if (!s_stream || !s_playback_done || s_playback_active) {
        return ESP_ERR_INVALID_STATE;
    }
    audio_pipeline_clear_cancel();
    s_stream_closed = false;
    xStreamBufferReset(s_stream);
    xSemaphoreTake(s_playback_done, 0); /* 清掉可能残留的信号 */
    s_playback_active = true;
    BaseType_t ok = xTaskCreatePinnedToCore(playback_task, "tts_playback_task",
                                            TTS_PLAYBACK_TASK_STACK, NULL, 4, NULL, 0);
    if (ok != pdPASS) {
        s_playback_active = false;
        ESP_LOGE(TAG, "播放任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    pcm_ctx_t ctx = { .cancel_flag = cancel_flag };
    esp_err_t err = tts_client_synthesize(text, on_pcm, &ctx, cancel_flag, out_status);
    if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
        ESP_LOGW(TAG, "TTS合成失败: %s status=%d", esp_err_to_name(err), out_status ? *out_status : -1);
    }
    s_stream_closed = true;

    /* 等播放任务把缓冲播完，保证调用方重启 wakeword 前音频已放完。若底层
     * codec 写永久阻塞，保留 active 标记拒绝复用共享流缓冲，避免两轮并发写。 */
    if (xSemaphoreTake(s_playback_done, pdMS_TO_TICKS(TTS_PLAYBACK_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "播放任务未在%dms内结束", TTS_PLAYBACK_WAIT_MS);
        audio_pipeline_cancel();
        return ESP_ERR_TIMEOUT;
    }
    audio_pipeline_clear_cancel();
    return err;
}
