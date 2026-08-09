/*
 * wakeword_loop.c - AFE/VAD 检测任务与语音段采集。
 * 任务仅在PTT对话轮期间运行(wakeword_start/stop管理)，平时无常驻监听。
 * 初始化和生命周期仍由 wakeword.c 管理。
 */
#include "wakeword_internal.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "wakeword";

static esp_err_t ensure_utterance_buf(void)
{
    if (g_wakeword_ctx.utterance_buf) {
        return ESP_OK;
    }
    size_t bytes = WAKEWORD_UTTERANCE_MAX_SAMPLES * sizeof(int16_t);
    g_wakeword_ctx.utterance_buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!g_wakeword_ctx.utterance_buf) {
        ESP_LOGE(TAG, "分配语音段缓冲失败(%u KB)", (unsigned)(bytes / 1024));
        return ESP_ERR_NO_MEM;
    }
    g_wakeword_ctx.utterance_cap = WAKEWORD_UTTERANCE_MAX_SAMPLES;
    ESP_LOGI(TAG, "语音段缓冲已分配: %u 样本 / %u KB PSRAM",
             (unsigned)WAKEWORD_UTTERANCE_MAX_SAMPLES, (unsigned)(bytes / 1024));
    return ESP_OK;
}

static bool append_utterance(const int16_t *data, size_t count)
{
    if (!g_wakeword_ctx.utterance_buf || count == 0) {
        return false;
    }
    size_t space = g_wakeword_ctx.utterance_cap - g_wakeword_ctx.utterance_count;
    size_t copy = count < space ? count : space;
    memcpy(g_wakeword_ctx.utterance_buf + g_wakeword_ctx.utterance_count,
           data, copy * sizeof(*data));
    g_wakeword_ctx.utterance_count += copy;
    return g_wakeword_ctx.utterance_count == g_wakeword_ctx.utterance_cap;
}

static void finish_utterance(bool capped)
{
    if (capped) {
        ESP_LOGW(TAG, "语音达到%u秒上限，按上限提交",
                 WAKEWORD_UTTERANCE_MAX_SECONDS);
    } else {
        ESP_LOGI(TAG, "语音结束 (共%u样本)",
                 (unsigned)g_wakeword_ctx.utterance_count);
    }
    if (g_wakeword_ctx.cb) {
        g_wakeword_ctx.cb(WAKEWORD_EVENT_SPEECH_END, g_wakeword_ctx.ctx);
    }
    g_wakeword_ctx.state = STATE_IDLE;
    g_wakeword_ctx.vad_was_active = false;
}

static void process_vad(const afe_fetch_result_t *res)
{
    if (g_wakeword_ctx.state != STATE_LISTENING) {
        return;
    }
    bool vad_active = res->vad_state == VAD_SPEECH;
    if (vad_active && !g_wakeword_ctx.vad_was_active) {
        if (ensure_utterance_buf() != ESP_OK) {
            /* 分配失败：不发SPEECH_START也不置vad_was_active，下一帧重试，
             * 避免带着上一轮残留缓冲提交陈旧录音 */
            return;
        }
        g_wakeword_ctx.utterance_count = 0;
        ESP_LOGI(TAG, "语音开始");
        /* VAD判定滞后，起始音节缓存在vad_cache里，先补进来避免句首截断 */
        if (res->vad_cache_size > 0) {
            append_utterance(res->vad_cache, (size_t)res->vad_cache_size / sizeof(int16_t));
        }
        if (g_wakeword_ctx.cb) {
            g_wakeword_ctx.cb(WAKEWORD_EVENT_SPEECH_START, g_wakeword_ctx.ctx);
        }
    }
    /* 采集AFE处理后的输出(res->data)而非原始feed帧：与VAD判定同帧对齐且已降噪 */
    if (vad_active && append_utterance(res->data, (size_t)res->data_size / sizeof(int16_t))) {
        finish_utterance(true);
        return;
    }
    if (!vad_active && g_wakeword_ctx.vad_was_active) {
        finish_utterance(false);
        return;
    }
    g_wakeword_ctx.vad_was_active = vad_active;
}

void wakeword_task(void *arg)
{
    (void)arg;
    int feed_chunksize = g_wakeword_ctx.afe_handle->get_feed_chunksize(g_wakeword_ctx.afe_data);
    int feed_nch = g_wakeword_ctx.afe_handle->get_feed_channel_num(g_wakeword_ctx.afe_data);
    size_t frame_samples = (size_t)feed_chunksize * feed_nch;
    int16_t *feed = heap_caps_malloc(frame_samples * sizeof(*feed), MALLOC_CAP_SPIRAM);
    if (!feed) {
        ESP_LOGE(TAG, "分配feed缓冲失败");
        g_wakeword_ctx.running = false;
        g_wakeword_ctx.task = NULL; /* stop()以句柄归零为退出依据 */
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "检测任务启动: feed_chunk=%d ch=%d fetch_chunk=%d", feed_chunksize, feed_nch,
             g_wakeword_ctx.afe_handle->get_fetch_chunksize(g_wakeword_ctx.afe_data));
    /* state/vad_was_active 由 wakeword_start 在创建本任务前复位，这里不重置，
     * 否则会把start后紧接着trigger注入的LISTENING冲掉(PTT丢失) */
    while (g_wakeword_ctx.running) {
        size_t got = audio_pipeline_read(feed, frame_samples, 1000);
        if (got == 0) {
            vTaskDelay(pdMS_TO_TICKS(10)); /* cancel/超时后避免优先级5任务忙旋饿死IDLE1 */
            continue;
        }
        if (got < frame_samples) {
            memset(feed + got, 0, (frame_samples - got) * sizeof(*feed));
        }
        g_wakeword_ctx.afe_handle->feed(g_wakeword_ctx.afe_data, feed);
        afe_fetch_result_t *res = g_wakeword_ctx.afe_handle->fetch_with_delay(
            g_wakeword_ctx.afe_data, 100 / portTICK_PERIOD_MS);
        if (!res || res->ret_value == ESP_FAIL) {
            ESP_LOGW(TAG, "fetch失败");
            continue;
        }
        process_vad(res);
    }
    heap_caps_free(feed);
    ESP_LOGI(TAG, "检测任务退出");
    g_wakeword_ctx.task = NULL; /* stop()以句柄归零为退出依据，见wakeword_stop */
    vTaskDelete(NULL);
}
