/* voice_tts_play.c - PCM 流播放会话：非流式 va_tts_play 与 reply 流式共用。
 * 从 voice_tts_stream.c 拆出压文件红线；播放任务独立于合成/网络接收，
 * 边收边放降低感知延迟。 */
#include "voice_assistant_internal.h"

#include <string.h>

#include "audio_pipeline.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "tts_client.h"
#include "tts_text.h"

static const char *TAG = "voice_tts_play";

#define TTS_STREAM_BUF_BYTES     (16 * 1024) /* ~0.5s@16kHz mono16bit，吸收网络抖动 */
#define TTS_STREAM_TRIGGER_BYTES 512
#define TTS_FIXED_GAIN_X10       50 /* 5.0x 固定增益，逐样本限幅防溢出 */
#define TTS_PLAYBACK_TASK_STACK  4096
#define TTS_PLAYBACK_WAIT_MS     20000 /* 等播放收尾上限，防调用方卡死 */

static StreamBufferHandle_t s_stream;
static SemaphoreHandle_t s_playback_done;
static volatile bool s_stream_closed;
static volatile bool s_playback_active;
static volatile esp_err_t s_playback_result;
static uint8_t *s_stream_buf;
static StaticStreamBuffer_t s_stream_state;
static volatile uint32_t s_playback_gen; /* 每轮 playback 递增；任务持快照，收尾时核对防旧任务污染新轮 */
static volatile int32_t s_playback_tasks; /* 存活 playback 任务数：start 等待归零防并发写 codec */

/* 独立播放任务：从流缓冲取数据写 codec，与网络接收(调用方任务)解耦。
 * 首次收到数据才开PA，收完(closed且缓冲已空)后关PA退出，避免频繁开关咔哒声 */
static void playback_task(void *arg)
{
    /* 创建时快照的代次：被强制复位后旧任务收尾不得再写共享状态/give信号，
     * 否则会污染新轮的 finish 等待与 playback 复用检查 */
    uint32_t my_gen = (uint32_t)(uintptr_t)arg;
    int16_t chunk[TTS_STREAM_TRIGGER_BYTES / sizeof(int16_t)];
    bool pa_on = false;

    for (;;) {
        /* 已被新轮取代(300s超时复位后新轮start递增gen并reset流)：
         * 立即退出，否则会读到新轮数据写codec与新播放任务双写 */
        if (s_playback_gen != my_gen) break;
        size_t got = xStreamBufferReceive(s_stream, chunk, sizeof(chunk), pdMS_TO_TICKS(200));
        /* 阻塞期间换代：不消费(流缓冲单读者，抢走即丢字) */
        if (s_playback_gen != my_gen) break;
        if (got > 0) {
            if (!pa_on) {
                audio_pipeline_set_output_enable(true);
                pa_on = true;
            }
            esp_err_t err = audio_pipeline_write(chunk, got / sizeof(int16_t));
            if (s_playback_gen != my_gen) break; /* 写入中换代：丢弃结果不污染新轮 */
            if (err != ESP_OK) {
                s_playback_result = err;
                ESP_LOGW(TAG, "音频写入失败: %s", esp_err_to_name(err));
                break;
            }
        } else if (s_stream_closed && xStreamBufferIsEmpty(s_stream)) {
            break;
        }
    }
    if (s_playback_gen == my_gen) {
        if (pa_on) {
            audio_pipeline_set_output_enable(false);
        }
        s_playback_active = false;
        xSemaphoreGive(s_playback_done);
    }
    s_playback_tasks--; /* 无条件递减：start 以此等待旧任务退出 */
    ESP_LOGI(TAG, "tts_playback_task 栈余 %uB",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    vTaskDeleteWithCaps(NULL);
}

/* tts_client 分片回调：只做固定增益+推入环形缓冲，播放在独立任务里进行。
 * 缓冲满(播放跟不上)或已取消时返回非OK，中止合成 */
esp_err_t tts_play_synth_cb(const int16_t *samples, size_t sample_count, void *ctx)
{
    tts_play_ctx_t *pc = ctx;
    /* 合成途中播放会话被强制复位换代：中止推入，避免旧合成污染新轮流 */
    if (pc->gen != s_playback_gen) {
        return ESP_ERR_NOT_FINISHED;
    }
    if (pc->cancel_flag && *pc->cancel_flag) {
        return ESP_ERR_NOT_FINISHED;
    }
    int16_t gained[128];
    size_t done = 0;
    while (done < sample_count) {
        /* send 前复查：检查后被抢占换代的窗口内不注入旧 PCM */
        if (pc->gen != s_playback_gen) {
            return ESP_ERR_NOT_FINISHED;
        }
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

/* 启动播放会话：playback_task 生命周期 + s_stream/s_playback_* 状态，
 * va_tts_play(非流式) 与 reply_stream_task(流式) 共用 */
esp_err_t tts_play_session_start(void)
{
    if (!s_stream || !s_playback_done || s_playback_active) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 旧播放任务未退出(300s超时复位残留)：有界等待退出，防新旧并发写 codec */
    for (int i = 0; i < 20 && s_playback_tasks > 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_playback_tasks > 0) {
        return ESP_ERR_INVALID_STATE; /* 旧任务卡死，止损：本轮播报失败 */
    }
    uint32_t my_gen = ++s_playback_gen; /* 先换代：任何状态变更前旧任务即失效 */
    audio_pipeline_clear_cancel(); /* 清除上轮 finish 超时残留的 cancel 标志 */
    /* reset 在旧任务阻塞于 receive 时返回 pdFAIL(ESP-IDF stream_buffer)不重置：
     * 短等重试，旧任务 200ms 周期醒来后 gen 检查自行退出。卡在网络合成/写入的
     * 旧任务不在 receive 上，首次 reset 即成功 */
    bool reset_ok = false;
    for (int i = 0; i < 5; i++) {
        if (xStreamBufferReset(s_stream) == pdPASS) {
            reset_ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!reset_ok) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_playback_done, 0);
    s_stream_closed = false;
    s_playback_result = ESP_OK;
    s_playback_active = true;
    s_playback_tasks++;
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(playback_task, "tts_playback_task",
                                            TTS_PLAYBACK_TASK_STACK, (void *)(uintptr_t)my_gen, 4, NULL, 0, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        s_playback_tasks--;
        s_playback_active = false;
        ESP_LOGE(TAG, "播放任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t tts_play_session_close(void)
{
    s_stream_closed = true;
    if (xSemaphoreTake(s_playback_done, pdMS_TO_TICKS(TTS_PLAYBACK_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "播放任务未在%dms内结束", TTS_PLAYBACK_WAIT_MS);
        audio_pipeline_cancel();
        s_playback_result = ESP_ERR_TIMEOUT;
    }
    audio_pipeline_clear_cancel();
    return s_playback_result;
}

/* finish 超时强制复位：唤醒卡住的 playback 并复位状态，供后续 turn 的 start 复用 */
void tts_play_session_reset(void)
{
    audio_pipeline_cancel();
    s_playback_active = false;
    s_stream_closed = true;
}

uint32_t tts_play_session_gen(void)
{
    return s_playback_gen;
}

esp_err_t va_tts_play(const char *text, const volatile bool *cancel_flag, int *out_status)
{
    if (out_status) *out_status = 0;
    if (!text) return ESP_ERR_INVALID_ARG;
    int discarded_status = 0;
    int *status = out_status ? out_status : &discarded_status;
    /* cancel 清除统一在 tts_play_session_start(等待旧任务退出后)执行，
     * 这里提前 clear 会重新放行超时残留的旧 playback */
    esp_err_t err = tts_play_session_start();
    if (err != ESP_OK) return err;
    tts_play_ctx_t ctx = { .cancel_flag = cancel_flag, .gen = s_playback_gen };
    err = tts_client_synthesize(text, tts_play_synth_cb, &ctx, cancel_flag, status);
    if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
        ESP_LOGW(TAG, "TTS合成失败: %s status=%d", esp_err_to_name(err), *status);
    }
    esp_err_t pb = tts_play_session_close();
    return pb != ESP_OK ? pb : err;
}

esp_err_t va_tts_play_clean(const char *text, const volatile bool *cancel_flag, int *out_status)
{
    if (out_status) *out_status = 0;
    if (!text) return ESP_ERR_INVALID_ARG;
    if (!tts_contains_markdown(text)) return va_tts_play(text, cancel_flag, out_status);
    size_t len = strlen(text);
    char *filtered = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    /* 过滤缓存不足时宁可失败，也不能退回原文把Markdown记号朗读出来。 */
    if (!filtered) return ESP_ERR_NO_MEM;
    size_t out_len = tts_strip_markdown(text, len, filtered);
    filtered[out_len] = '\0';
    if (out_len == 0) {
        if (out_status) *out_status = 0;
        heap_caps_free(filtered);
        return ESP_OK;
    }
    esp_err_t err = va_tts_play(filtered, cancel_flag, out_status);
    heap_caps_free(filtered);
    return err;
}
