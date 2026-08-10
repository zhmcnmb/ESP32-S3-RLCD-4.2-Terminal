#include "voice_assistant.h"
#include "voice_assistant_internal.h"
#include <inttypes.h>
#include "asr_client.h"
#include "audio_pipeline.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sys_state.h"
#include "wakeword.h"
#include "voice_phase.h"
static const char *TAG = "voice_assistant";
#define MAX_UTTERANCE_SAMPLES (15 * 16000)
/* ASR 文本进入 LLM 前只短暂驻留任务栈，512B 覆盖客户端单轮转写契约。 */
#define ASR_TEXT_MAX 512
#define LISTENING_TIMEOUT_MS 10000
static QueueHandle_t s_evt_queue;
static uint32_t s_turn_id;
static volatile bool s_cancel;
static volatile bool s_busy;    /* 一轮 ASR/LLM/TTS 进行中，忽略新唤醒 */
static volatile bool s_in_turn; /* 从 WAKE 到 va_finish_turn 期间为 true，防止重复唤醒 */
static int16_t *s_utt_buf;
static esp_timer_handle_t s_cancel_timer;
static int64_t s_listening_deadline_us;

static void clear_cancel_flags(void)
{
    s_cancel = false;
    /* wakeword 录音依赖 audio_pipeline_read：上轮取消(cancel)残留不清除会让
     * 唤醒词 read 恒返回0。此处清除不影响已退出的旧播放任务(被 cancel 中断的
     * write 必返回 NOT_FINISHED 后 break)，保留勿删 */
    audio_pipeline_clear_cancel();
    (void)sys_state_consume_voice_cancel();
}

static void poll_cancel(void)
{
    if (sys_state_consume_voice_cancel()) {
        s_cancel = true;
        /* LISTENING 时 wakeword 仍独占麦克风；此时中断 I2S 会让它永久等不到
         * SPEECH_END。云端阶段已停 wakeword，才允许直接打断阻塞音频 I/O。 */
        if (s_busy) {
            audio_pipeline_cancel();
        }
    }
}

/* ASR/LLM/TTS 阻塞期间也要响应 BOOT 取消：定时从 sys_state 取请求位 */
static void cancel_timer_cb(void *arg)
{
    (void)arg;
    poll_cancel();
}

/* ---- voice_text_turn.c 共享的轮次原语 ---- */

/* 新一轮开始：占位防重入、发轮次号、清取消标志；返回新轮次号 */
uint32_t va_turn_next(void)
{
    s_in_turn = true;
    s_busy = true;
    s_turn_id++;
    voice_phase_set_turn(s_turn_id);
    clear_cancel_flags();
    return s_turn_id;
}

uint32_t va_turn_id(void)
{
    return s_turn_id;
}

volatile bool *va_turn_cancel_flag(void)
{
    return &s_cancel;
}

/* wakeword 事件回调只投递队列，处理全在 voice_task 上下文 */
static void wakeword_cb(wakeword_event_t event, void *ctx)
{
    (void)ctx;
    if (s_evt_queue) {
        xQueueSend(s_evt_queue, &event, 0);
    }
}

static esp_err_t run_asr(char *out_text, size_t out_len)
{
    voice_alert(SYS_VOICE_TRANSCRIBING, NULL);
    int status = 0;
    esp_err_t err = asr_client_transcribe(WAV_REL_PATH, out_text, out_len, &s_cancel, &status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ASR失败: %s status=%d", esp_err_to_name(err), status);
    }
    return err;
}

/* 提醒仅在语音任务空闲时接手；音频独占和播报细节留在 voice_reminder.c。 */
static void reminder_started(void)
{
    s_in_turn = true;
    s_busy = true;
    s_turn_id++;
    voice_phase_set_turn(s_turn_id);
    voice_alert(SYS_VOICE_SPEAKING, NULL);
}

static void reminder_finished(void)
{
    va_finish_turn(SYS_VOICE_IDLE, NULL);
}

static void play_pending_reminder(void)
{
    va_reminder_play(reminder_started, reminder_finished, &s_cancel);
}

/* 检查阶段结果：取消→IDLE，错误→ERROR(带原因)。返回 true 表示 turn 已结束 */
bool va_stage_failed(esp_err_t err, const char *fail_reason)
{
    poll_cancel();
    if (s_cancel || err == ESP_ERR_NOT_FINISHED) {
        va_finish_turn(SYS_VOICE_IDLE, NULL);
        return true;
    }
    if (err != ESP_OK) {
        va_finish_turn(SYS_VOICE_ERROR, fail_reason);
        return true;
    }
    return false;
}
static bool listening_timed_out(void)
{
    return s_listening_deadline_us != 0 && esp_timer_get_time() >= s_listening_deadline_us;
}
static void finish_listening(void)
{
    esp_err_t err = wakeword_stop();
    if (err != ESP_OK) {
        /* 停不下来也必须收轮回IDLE，否则s_in_turn永真、状态机卡死LISTENING */
        ESP_LOGE(TAG, "监听停止失败: %s", esp_err_to_name(err));
        va_finish_turn(SYS_VOICE_ERROR, "监听停止失败");
        return;
    }
    va_finish_turn(SYS_VOICE_IDLE, NULL);
}

void va_finish_turn(sys_voice_phase_t phase, const char *reason)
{
    ESP_LOGI(TAG, "turn %" PRIu32 " 栈余 %uB", s_turn_id,
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    voice_alert(phase, reason);
    s_listening_deadline_us = 0;
    clear_cancel_flags();
    if (phase != SYS_VOICE_IDLE) {
        /* 错误态短暂停留后回 IDLE，避免头像永久故障 */
        vTaskDelay(pdMS_TO_TICKS(1500));
        voice_alert(SYS_VOICE_IDLE, NULL);
        clear_cancel_flags();
    }
    /* 清空处理期间积压的事件；PTT-only下管线保持停止，下一轮由按键重新拉起 */
    if (s_evt_queue) {
        wakeword_event_t drop;
        while (xQueueReceive(s_evt_queue, &drop, 0) == pdTRUE) {
        }
    }
    s_busy = false;
    s_in_turn = false;
}

static void handle_speech_end(void)
{
    /* 立即停录音管线，本轮后续(ASR/LLM/TTS)不再需要麦克风 */
    s_busy = true;
    esp_err_t err = wakeword_stop();
    if (err != ESP_OK) {
        va_finish_turn(SYS_VOICE_ERROR, "监听停止失败");
        return;
    }
    poll_cancel();
    if (s_cancel) {
        va_finish_turn(SYS_VOICE_IDLE, NULL);
        return;
    }
    size_t n = wakeword_get_utterance(s_utt_buf, MAX_UTTERANCE_SAMPLES);
    if (n == 0) {
        ESP_LOGW(TAG, "turn %" PRIu32 " 录音为空", s_turn_id);
        va_finish_turn(SYS_VOICE_IDLE, NULL);
        return;
    }
    size_t trimmed = va_wav_trim_trailing_silence(s_utt_buf, n);
    if (trimmed < n) {
        ESP_LOGI(TAG, "turn %" PRIu32 " 裁剪尾部静音 %u->%u样本",
                 s_turn_id, (unsigned)n, (unsigned)trimmed);
        n = trimmed;
    }
    int32_t peak = va_wav_normalize_utterance(s_utt_buf, n);
    err = va_wav_save_utterance(s_utt_buf, n);
    if (err != ESP_OK) {
        va_finish_turn(SYS_VOICE_ERROR, "录音失败");
        return;
    }
    ESP_LOGI(TAG, "turn %" PRIu32 " WAV %u样本 峰值%d", s_turn_id, (unsigned)n, (int)peak);
    char asr_text[ASR_TEXT_MAX];
    err = run_asr(asr_text, sizeof(asr_text));
    if (va_stage_failed(err, "识别失败")) return;
    if (asr_text[0] == '\0') {
        va_finish_turn(SYS_VOICE_ERROR, "未听清");
        return;
    }
    ESP_LOGI(TAG, "turn %" PRIu32 " ASR: %s", s_turn_id, asr_text);
    const char *reply = NULL;
    bool streamed = false;
    err = va_run_chat(asr_text, &reply, &streamed);
    if (va_stage_failed(err, "回答失败")) return;
    if (va_play_full_reply(reply, streamed)) return;
    va_finish_turn(SYS_VOICE_IDLE, NULL);
}

static void dispatch_wake_event(wakeword_event_t evt)
{
    switch (evt) {
    case WAKEWORD_EVENT_WAKE:
        if (s_in_turn) {
            break; /* 上一轮未结束，忽略重复触发 */
        }
        s_in_turn = true;
        s_turn_id++;
        voice_phase_set_turn(s_turn_id);
        clear_cancel_flags();
        voice_alert(SYS_VOICE_LISTENING, NULL);
        ESP_LOGI(TAG, "turn %" PRIu32 " 触发(PTT)", s_turn_id);
        s_listening_deadline_us = esp_timer_get_time() + (int64_t)LISTENING_TIMEOUT_MS * 1000;
        break;
    case WAKEWORD_EVENT_SPEECH_START:
        if (s_in_turn) {
            voice_alert(SYS_VOICE_LISTENING, NULL);
            s_listening_deadline_us = 0;
        }
        break;
    case WAKEWORD_EVENT_SPEECH_END:
        if (s_in_turn) {
            handle_speech_end();
        }
        break;
    default:
        break;
    }
}

static void voice_task(void *arg)
{
    (void)arg;
    s_utt_buf = heap_caps_malloc(MAX_UTTERANCE_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_utt_buf) {
        ESP_LOGE(TAG, "录音缓冲分配失败");
        vTaskDelete(NULL);
        return;
    }
    esp_err_t err = wakeword_init(wakeword_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wakeword_init失败: %s", esp_err_to_name(err));
        heap_caps_free(s_utt_buf);
        s_utt_buf = NULL;
        vTaskDelete(NULL);
        return;
    }
    /* 无常驻监听：录音管线由PTT触发时才wakeword_start拉起，轮结束即停 */
    voice_alert(SYS_VOICE_IDLE, NULL);
    ESP_LOGI(TAG, "就绪，按键说话 | 栈余 %" PRIu32 "B",
             (uint32_t)uxTaskGetStackHighWaterMark(NULL));
    wakeword_event_t evt;
    for (;;) {
        BaseType_t received = xQueueReceive(s_evt_queue, &evt, pdMS_TO_TICKS(100));
        poll_cancel();
        if (!s_busy && s_in_turn && (s_cancel || listening_timed_out())) {
            if (s_cancel) {
                ESP_LOGI(TAG, "turn %" PRIu32 " 取消监听", s_turn_id);
            } else {
                ESP_LOGI(TAG, "turn %" PRIu32 " 等待说话超时", s_turn_id);
            }
            finish_listening();
            continue;
        }
        if (received == pdTRUE && !s_busy) {
            dispatch_wake_event(evt);
        } else if (!s_busy && !s_in_turn) {
            if (sys_state_consume_voice_trigger()) {
                /* push-to-talk: 拉起录音管线再注入WAKE,
                 * 下一轮 dispatch_wake_event 接管 */
                esp_err_t err = wakeword_start();
                if (err == ESP_OK) {
                    err = wakeword_trigger();
                    if (err != ESP_OK) {
                        (void)wakeword_stop(); /* 触发失败不留空转任务 */
                    }
                }
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "手动触发失败: %s", esp_err_to_name(err));
                }
                continue;
            }
            char text[ASR_TEXT_MAX];
            if (sys_state_consume_text_turn(text, sizeof(text))) {
                va_handle_text_turn(text);
                continue;
            }
            play_pending_reminder();
        }
    }
}

esp_err_t voice_assistant_start(void)
{
    if (!audio_pipeline_is_ready()) {
        ESP_LOGW(TAG, "音频链路未就绪，跳过语音助手");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_evt_queue) {
        s_evt_queue = xQueueCreate(8, sizeof(wakeword_event_t));
        ESP_RETURN_ON_FALSE(s_evt_queue, ESP_ERR_NO_MEM, TAG, "queue create failed");
    }
    ESP_RETURN_ON_ERROR(va_tts_stream_init(), TAG, "tts stream init");
    if (!s_cancel_timer) {
        const esp_timer_create_args_t args = {
            .callback = cancel_timer_cb,
            .name = "va_cancel",
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_cancel_timer), TAG, "cancel timer");
        /* 100ms 轮询足够响应 BOOT 长按取消，又不会抢 CPU */
        ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_cancel_timer, 100 * 1000), TAG, "cancel timer start");
    }
    /* 固定 CPU0：wakeword 独占 CPU1(prio5)，voice_task 不能与之同核否则被饿死；
     * 且 ASR/LLM/TTS 走 WiFi/lwIP(CPU0)，同核减少跨核开销 */
    BaseType_t ok = xTaskCreatePinnedToCore(voice_task, "voice_assistant_task", 12288, NULL, 4, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");
    return ESP_OK;
}
