#include "voice_assistant_internal.h"

#include <ctype.h>

#include <string.h>


#include "audio_pipeline.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "tts_text.h"
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
#define REPLY_STREAM_BUF_BYTES    (16 * 1024)
#define REPLY_STREAM_READ_BYTES   256

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

static StreamBufferHandle_t s_reply_stream;
static SemaphoreHandle_t s_reply_done;
static uint8_t *s_reply_stream_buf;
static StaticStreamBuffer_t s_reply_stream_state;
static const volatile bool *s_reply_cancel_flag;
static volatile bool s_reply_closed;
static volatile bool s_reply_active;
static volatile bool s_reply_has_content;
static volatile esp_err_t s_reply_result;
static volatile bool s_reply_task_started; /* lazy start: 首个 delta 才起 TTS 任务 */

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
                s_playback_result = err;
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
    ESP_LOGI(TAG, "tts_playback_task 栈余 %uB",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    vTaskDeleteWithCaps(NULL);
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

/* playback session：封装 playback_task 生命周期 + s_stream/s_playback_* 状态，
 * va_tts_play(非流式) 与 reply_stream_task(流式) 共用，消除双套启停样板。 */
static esp_err_t playback_session_start(void)
{
    if (!s_stream || !s_playback_done || s_playback_active) {
        return ESP_ERR_INVALID_STATE;
    }
    audio_pipeline_clear_cancel(); /* 清除上轮 finish 超时残留的 cancel 标志 */
    xStreamBufferReset(s_stream);
    xSemaphoreTake(s_playback_done, 0);
    s_stream_closed = false;
    s_playback_result = ESP_OK;
    s_playback_active = true;
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(playback_task, "tts_playback_task",
                                            TTS_PLAYBACK_TASK_STACK, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        s_playback_active = false;
        ESP_LOGE(TAG, "播放任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t playback_session_close(void)
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

esp_err_t va_tts_play(const char *text, const volatile bool *cancel_flag, int *out_status)
{
    if (out_status) *out_status = 0;
    if (!text) return ESP_ERR_INVALID_ARG;
    int discarded_status = 0;
    int *status = out_status ? out_status : &discarded_status;
    audio_pipeline_clear_cancel();
    esp_err_t err = playback_session_start();
    if (err != ESP_OK) return err;
    pcm_ctx_t ctx = { .cancel_flag = cancel_flag };
    err = tts_client_synthesize(text, on_pcm, &ctx, cancel_flag, status);
    if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
        ESP_LOGW(TAG, "TTS合成失败: %s status=%d", esp_err_to_name(err), *status);
    }
    esp_err_t pb = playback_session_close();
    return pb != ESP_OK ? pb : err;
}

esp_err_t va_tts_play_clean(const char *text, const volatile bool *cancel_flag, int *out_status)
{
    if (out_status) *out_status = 0;
    if (!text) return ESP_ERR_INVALID_ARG;
    if (!strpbrk(text, "*`#_")) return va_tts_play(text, cancel_flag, out_status);
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


static esp_err_t reply_speak(char *pending, size_t *pending_len, size_t sentence_len)
{
    size_t consumed_len = sentence_len;
    esp_err_t err = ESP_OK;
    size_t start = 0;
    while (start < sentence_len && isspace((unsigned char)pending[start])) start++;
    while (sentence_len > start && isspace((unsigned char)pending[sentence_len - 1])) {
        sentence_len--;
    }
    if (sentence_len > start) {
        /* 仅部分消费时才需保护下一句首字节；整块消费后读边界外的未初始化字节没有意义。 */
        bool has_tail = sentence_len < *pending_len;
        char saved = has_tail ? pending[sentence_len] : '\0';
        pending[sentence_len] = '\0';
        int status = 0;
        pcm_ctx_t pctx = { .cancel_flag = s_reply_cancel_flag };
        err = tts_client_synthesize(pending + start, on_pcm, &pctx, s_reply_cancel_flag, &status);
        if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
            ESP_LOGW(TAG, "TTS合成失败: %s status=%d", esp_err_to_name(err), status);
        }
        if (has_tail) pending[sentence_len] = saved;
    }
    if (err != ESP_OK) return err;
    memmove(pending, pending + consumed_len, *pending_len - consumed_len);
    *pending_len -= consumed_len;
    return ESP_OK;
}

static void reply_stream_task(void *arg)
{
    (void)arg;
    char incoming[REPLY_STREAM_READ_BYTES];
    char pending[TTS_SENTENCE_MAX + 1];
    size_t pending_len = 0;

    /* 启动持续播放任务：跨多句合成复用同一 playback，合成下一句时上一句仍在播，
     * 消除"每句等播完再合成"的句间停顿(va_tts_play 的 per-call reset+wait) */
    if (playback_session_start() != ESP_OK) {
        s_reply_active = false;
        s_reply_result = ESP_ERR_NO_MEM;
        xSemaphoreGive(s_reply_done);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    for (;;) {
        size_t got = xStreamBufferReceive(s_reply_stream, incoming, sizeof(incoming),
                                          pdMS_TO_TICKS(100));
        if (got > 0) {
            if (pending_len + got > TTS_SENTENCE_MAX) {
                s_reply_result = reply_speak(pending, &pending_len,
                                              tts_safe_chunk_len(pending, pending_len));
                if (s_reply_result != ESP_OK) break;
            }
            memcpy(pending + pending_len, incoming, got);
            pending_len += got;
            size_t chunk_len = tts_sentences_len(pending, pending_len);
            if (chunk_len > 0) {
                s_reply_result = reply_speak(pending, &pending_len, chunk_len);
            }
            if (s_reply_result != ESP_OK) break;
        }
        if (s_reply_closed && xStreamBufferIsEmpty(s_reply_stream)) {
            if (pending_len > 0) {
                s_reply_result = reply_speak(pending, &pending_len, pending_len);
            }
            break;
        }
    }
    /* 收尾：等播放任务播完缓冲内剩余音频；reply 合成错优先，否则记 playback 错 */
    esp_err_t pb = playback_session_close();
    if (pb != ESP_OK && s_reply_result == ESP_OK) {
        s_reply_result = pb;
    }
    s_reply_active = false;
    xSemaphoreGive(s_reply_done);
    ESP_LOGI(TAG, "reply_tts_task 栈余 %uB",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    vTaskDeleteWithCaps(NULL);
}

esp_err_t va_reply_stream_start(const volatile bool *cancel_flag)
{
    if (!s_reply_stream) {
        s_reply_stream_buf = heap_caps_malloc(REPLY_STREAM_BUF_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_reply_stream_buf) return ESP_ERR_NO_MEM;
        s_reply_stream = xStreamBufferCreateStatic(REPLY_STREAM_BUF_BYTES, 1, s_reply_stream_buf,
                                                    &s_reply_stream_state);
        if (!s_reply_stream) {
            heap_caps_free(s_reply_stream_buf);
            s_reply_stream_buf = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_reply_done) {
        s_reply_done = xSemaphoreCreateBinary();
        if (!s_reply_done) return ESP_ERR_NO_MEM;
    }
    if (s_reply_active) return ESP_ERR_INVALID_STATE;
    xStreamBufferReset(s_reply_stream);
    xSemaphoreTake(s_reply_done, 0);
    s_reply_cancel_flag = cancel_flag;
    s_reply_closed = false;
    s_reply_has_content = false;
    s_reply_result = ESP_OK;
    s_reply_task_started = false;
    s_reply_active = true;
    /* reply_tts_task 延迟到首个 delta 才启动：LLM 工具回合无 delta 时空占 6KB
     * 内部栈，挤压多轮 TLS 的 SSL context 内存(IN_CONTENT_LEN=16KB) */
    return ESP_OK;
}


#define REPLY_FILTER_WINDOW 128

esp_err_t va_reply_stream_push_delta(const char *delta)
{
    if (!delta || !s_reply_active || s_reply_closed) return ESP_ERR_INVALID_STATE;
    if (!s_reply_task_started) {
        s_reply_task_started = true;
        BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(reply_stream_task, "reply_tts_task", 6144,
                                                NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
        if (ok != pdPASS) {
            s_reply_result = ESP_ERR_NO_MEM;
            return ESP_ERR_NO_MEM;
        }
    }
    size_t remaining = strlen(delta);
    const char *cursor = delta;
    char filtered[REPLY_FILTER_WINDOW];
    while (remaining > 0) {
        if (s_reply_cancel_flag && *s_reply_cancel_flag) return ESP_ERR_NOT_FINISHED;
        if (s_reply_result != ESP_OK) return s_reply_result;
        size_t window = remaining > REPLY_FILTER_WINDOW ? REPLY_FILTER_WINDOW : remaining;
        size_t filtered_len = tts_strip_markdown(cursor, window, filtered);
        size_t sent_total = 0;
        while (sent_total < filtered_len) {
            size_t sent = xStreamBufferSend(s_reply_stream, filtered + sent_total,
                                            filtered_len - sent_total, pdMS_TO_TICKS(100));
            sent_total += sent;
            if (sent == 0) {
                if (s_reply_cancel_flag && *s_reply_cancel_flag) return ESP_ERR_NOT_FINISHED;
                if (s_reply_result != ESP_OK) return s_reply_result;
            }
        }
        if (filtered_len) s_reply_has_content = true;
        cursor += window;
        remaining -= window;
    }
    return ESP_OK;
}

bool va_reply_stream_has_content(void)
{
    return s_reply_has_content;
}

esp_err_t va_reply_stream_finish(void)
{
    if (!s_reply_stream || !s_reply_done) return ESP_ERR_INVALID_STATE;
    if (!s_reply_active && !s_reply_closed) return s_reply_result;
    s_reply_closed = true;
    /* 无 delta -> reply_tts_task 未启动，无 TTS 可收尾，直接复位 */
    if (!s_reply_task_started) {
        s_reply_active = false;
        return s_reply_result;
    }
    /* 有界等待：每句合成/播放各自有超时，正常远小于60s。异常超时后
     * s_reply_active仍由任务自身清零，未退出前start()拒绝复用，不会并发写流 */
    if (xSemaphoreTake(s_reply_done, pdMS_TO_TICKS(300000)) != pdTRUE) {
        ESP_LOGE(TAG, "reply_tts_task 未在300s内结束，强制复位");
        /* cancel 唤醒卡住的 playback_task，复位状态让后续 turn 的 start 能用，
         * 旧任务泄漏但不阻塞新一轮语音(否则 s_reply_active 永真致连续回答失败) */
        audio_pipeline_cancel();
        s_reply_task_started = false;
        s_playback_active = false;
        s_stream_closed = true;
        s_reply_active = false;
        return ESP_ERR_TIMEOUT;
    }
    return s_reply_result;
}
