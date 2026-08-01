#include "voice_assistant_internal.h"

#include <ctype.h>

#include <string.h>


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
#define REPLY_STREAM_BUF_BYTES    (16 * 1024)
#define REPLY_STREAM_READ_BYTES   256

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
static volatile uint32_t s_reply_gen; /* 每轮 start 递增；任务持快照，收尾时核对防旧任务污染新轮 */
static volatile int32_t s_reply_tasks; /* 存活 reply 任务数：start 等待归零防跨代 receive 丢字 */



static esp_err_t reply_speak(char *pending, size_t *pending_len, size_t sentence_len,
                              uint32_t play_gen)
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
        /* gen 用本任务启动会话时的播放代次快照(调用方传入)，不能实时读：
         * 换代后实时值是新代次，synth_cb 的 pc->gen 校验会失效 */
        tts_play_ctx_t pctx = { .cancel_flag = s_reply_cancel_flag, .gen = play_gen };
        err = tts_client_synthesize(pending + start, tts_play_synth_cb, &pctx, s_reply_cancel_flag, &status);
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

/* 合成一句并写共享结果；合成中换代则丢弃结果(不污染新轮)。
 * 返回 false 表示应退出循环(换代或合成失败) */
static bool reply_speak_guarded(char *pending, size_t *pending_len, size_t sentence_len,
                                uint32_t my_gen, uint32_t play_gen)
{
    esp_err_t r = reply_speak(pending, pending_len, sentence_len, play_gen);
    if (s_reply_gen != my_gen) return false;
    s_reply_result = r;
    return s_reply_result == ESP_OK;
}

static void reply_stream_task(void *arg)
{
    /* 代次快照：被强制复位后旧任务收尾不得写共享状态/give信号 */
    uint32_t my_gen = (uint32_t)(uintptr_t)arg;
    char incoming[REPLY_STREAM_READ_BYTES];
    char pending[TTS_SENTENCE_MAX + 1];
    size_t pending_len = 0;

    /* 启动持续播放任务：跨多句合成复用同一 playback，合成下一句时上一句仍在播，
     * 消除"每句等播完再合成"的句间停顿(va_tts_play 的 per-call reset+wait) */
    if (tts_play_session_start() != ESP_OK) {
        s_reply_active = false;
        s_reply_result = ESP_ERR_NO_MEM;
        xSemaphoreGive(s_reply_done);
        s_reply_tasks--;
        vTaskDeleteWithCaps(NULL);
        return;
    }
    /* 播放代次快照：合成回调 gen 校验用本任务启动的代，实时读会拿到新代 */
    uint32_t play_gen = tts_play_session_gen();

    for (;;) {
        if (s_reply_gen != my_gen) break;
        size_t got = xStreamBufferReceive(s_reply_stream, incoming, sizeof(incoming),
                                          pdMS_TO_TICKS(100));
        /* 阻塞期间换代不消费(流缓冲单读者，抢走即丢字) */
        if (s_reply_gen != my_gen) break;
        if (got > 0) {
            if (pending_len + got > TTS_SENTENCE_MAX
                && !reply_speak_guarded(pending, &pending_len,
                                        tts_safe_chunk_len(pending, pending_len), my_gen, play_gen)) {
                break;
            }
            memcpy(pending + pending_len, incoming, got);
            pending_len += got;
            size_t chunk_len = tts_sentences_len(pending, pending_len);
            if (chunk_len > 0 && !reply_speak_guarded(pending, &pending_len, chunk_len, my_gen, play_gen)) {
                break;
            }
        }
        if (s_reply_closed && xStreamBufferIsEmpty(s_reply_stream)) {
            if (pending_len > 0) {
                reply_speak_guarded(pending, &pending_len, pending_len, my_gen, play_gen);
            }
            break;
        }
    }
    /* 收尾：仅当前代才收尾播放并写共享状态；旧任务直接退出 */
    if (s_reply_gen == my_gen) {
        esp_err_t pb = tts_play_session_close();
        if (pb != ESP_OK && s_reply_result == ESP_OK) s_reply_result = pb;
        s_reply_active = false;
        xSemaphoreGive(s_reply_done);
    }
    s_reply_tasks--; /* start 以此等待旧任务退出 */
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
    /* 旧 reply 任务未退出(300s超时残留)：有界等待退出，防跨代 receive
     * 拿走新轮字节(流缓冲单读者，取走即丢字) */
    for (int i = 0; i < 20 && s_reply_tasks > 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_reply_tasks > 0) return ESP_ERR_INVALID_STATE;
    ++s_reply_gen; /* 先换代：任何状态变更前旧任务即失效 */
    /* reset 在旧任务阻塞于 receive 时返回 pdFAIL(ESP-IDF stream_buffer)不重置：
     * 短等重试，旧任务 100ms 周期醒来后 gen 检查自行退出。卡在网络合成/写入的
     * 旧任务不在 receive 上，首次 reset 即成功 */
    bool reset_ok = false;
    for (int i = 0; i < 5; i++) {
        if (xStreamBufferReset(s_reply_stream) == pdPASS) {
            reset_ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!reset_ok) return ESP_ERR_INVALID_STATE;
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
        uint32_t my_gen = s_reply_gen;
        s_reply_tasks++;
        BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(reply_stream_task, "reply_tts_task", 6144,
                                                (void *)(uintptr_t)my_gen, 4, NULL, 0, MALLOC_CAP_SPIRAM);
        if (ok != pdPASS) {
            s_reply_tasks--;
            s_reply_result = ESP_ERR_NO_MEM;
            return ESP_ERR_NO_MEM;
        }
        /* 创建成功才置位：失败时 finish 走"未启动"快速路径立即复位 */
        s_reply_task_started = true;
    }
    size_t remaining = strlen(delta);
    const char *cursor = delta;
    char filtered[REPLY_FILTER_WINDOW];
    while (remaining > 0) {
        if (s_reply_cancel_flag && *s_reply_cancel_flag) return ESP_ERR_NOT_FINISHED;
        if (s_reply_result != ESP_OK) return s_reply_result;
        size_t window = remaining > REPLY_FILTER_WINDOW ? REPLY_FILTER_WINDOW : remaining;
        /* "- " 配对跨窗口时会把减号读出来：窗口尾是 '-' 且有后续内容时
         * 让给下一窗口一起过滤 */
        if (remaining > window && cursor[window - 1] == '-') {
            window--;
        }
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
        tts_play_session_reset();
        s_reply_task_started = false;
        s_reply_active = false;
        return ESP_ERR_TIMEOUT;
    }
    return s_reply_result;
}
