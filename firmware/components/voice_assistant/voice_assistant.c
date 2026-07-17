#include "voice_assistant.h"
#include "voice_assistant_internal.h"

#include <inttypes.h>

#include "agent_runtime.h"
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

static const char *TAG = "voice_assistant";

#define MAX_UTTERANCE_SAMPLES (12 * 16000)
/* ASR 文本进入 LLM 前只短暂驻留任务栈，512B 覆盖客户端单轮转写契约。 */
#define ASR_TEXT_MAX 512
#define LISTENING_TIMEOUT_MS 10000

static QueueHandle_t s_evt_queue;
static uint32_t s_turn_id;
static volatile bool s_cancel;
static volatile bool s_busy;    /* 一轮 ASR/LLM/TTS 进行中，忽略新唤醒 */
static volatile bool s_in_turn; /* 从 WAKE 到 finish_turn 期间为 true，防止重复唤醒 */
static int16_t *s_utt_buf;
static esp_timer_handle_t s_cancel_timer;
static sys_voice_phase_t s_current_phase = SYS_VOICE_IDLE;
static int64_t s_listening_deadline_us;

/* ---------- 状态机：只允许语义合理的相位转换，非法转换拒绝+记日志 ----------
 * P6.0 会启用 EXECUTING/CONFIRM，转换表提前留好分支，页面外部行为不变 */
static bool phase_transition_valid(sys_voice_phase_t from, sys_voice_phase_t to)
{
    if (from == to) {
        return true;
    }
    switch (from) {
    case SYS_VOICE_IDLE:
        return to == SYS_VOICE_LISTENING;
    case SYS_VOICE_LISTENING:
        return to == SYS_VOICE_TRANSCRIBING || to == SYS_VOICE_IDLE || to == SYS_VOICE_ERROR;
    case SYS_VOICE_TRANSCRIBING:
        return to == SYS_VOICE_THINKING || to == SYS_VOICE_IDLE || to == SYS_VOICE_ERROR;
    case SYS_VOICE_THINKING:
        return to == SYS_VOICE_SPEAKING || to == SYS_VOICE_EXECUTING
            || to == SYS_VOICE_IDLE || to == SYS_VOICE_ERROR;
    case SYS_VOICE_EXECUTING:
        return to == SYS_VOICE_SPEAKING || to == SYS_VOICE_THINKING
            || to == SYS_VOICE_IDLE || to == SYS_VOICE_ERROR;
    case SYS_VOICE_SPEAKING:
        return to == SYS_VOICE_IDLE || to == SYS_VOICE_ERROR;
    case SYS_VOICE_CONFIRM:
        return to == SYS_VOICE_EXECUTING || to == SYS_VOICE_IDLE || to == SYS_VOICE_ERROR;
    case SYS_VOICE_ERROR:
        return to == SYS_VOICE_IDLE;
    default:
        return false;
    }
}

/* 复合状态更新：校验转换合法性 + 推进相位 + (仅ERROR)带短原因文案，
 * page_jarvis 据此区分"网络故障"/"未听清"等，而不是所有异常显示同一个词 */
static void voice_alert(sys_voice_phase_t phase, const char *reason)
{
    if (!phase_transition_valid(s_current_phase, phase)) {
        ESP_LOGW(TAG, "非法状态转换 %d->%d，已拒绝", s_current_phase, phase);
        return;
    }
    s_current_phase = phase;
    sys_state_apply_voice(phase, s_turn_id, reason);
}

static void clear_cancel_flags(void)
{
    s_cancel = false;
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

static void on_agent_tool_state(bool active, void *ctx)
{
    (void)ctx;
    voice_alert(active ? SYS_VOICE_EXECUTING : SYS_VOICE_THINKING, NULL);
}

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

static esp_err_t run_chat(const char *user_text, const char **reply)
{
    voice_alert(SYS_VOICE_THINKING, NULL);
    esp_err_t err = agent_runtime_run_turn(user_text, &s_cancel,
                                           on_agent_tool_state, NULL, reply);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "智能体失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "turn %" PRIu32 " LLM: %s", s_turn_id, *reply);
    return ESP_OK;
}

static esp_err_t run_tts(const char *text)
{
    voice_alert(SYS_VOICE_SPEAKING, NULL);
    int status = 0;
    return va_tts_play(text, &s_cancel, &status);
}

static void finish_turn(sys_voice_phase_t phase, const char *reason);

/* 检查阶段结果：取消→IDLE，错误→ERROR(带原因)。返回 true 表示 turn 已结束 */
static bool stage_failed(esp_err_t err, const char *fail_reason)
{
    poll_cancel();
    if (s_cancel || err == ESP_ERR_NOT_FINISHED) {
        finish_turn(SYS_VOICE_IDLE, NULL);
        return true;
    }
    if (err != ESP_OK) {
        finish_turn(SYS_VOICE_ERROR, fail_reason);
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
        ESP_LOGE(TAG, "监听停止失败: %s", esp_err_to_name(err));
        return;
    }
    finish_turn(SYS_VOICE_IDLE, NULL);
}

static void finish_turn(sys_voice_phase_t phase, const char *reason)
{
    voice_alert(phase, reason);
    s_listening_deadline_us = 0;
    clear_cancel_flags();
    if (phase != SYS_VOICE_IDLE) {
        /* 错误态短暂停留后回 IDLE，避免头像永久故障 */
        vTaskDelay(pdMS_TO_TICKS(1500));
        voice_alert(SYS_VOICE_IDLE, NULL);
        clear_cancel_flags();
    }
    /* 清空处理期间积压的唤醒事件，再恢复检测 */
    if (s_evt_queue) {
        wakeword_event_t drop;
        while (xQueueReceive(s_evt_queue, &drop, 0) == pdTRUE) {
        }
    }
    s_busy = false;
    s_in_turn = false;
    esp_err_t err = wakeword_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "finish_turn: wakeword_start 失败: %s", esp_err_to_name(err));
    }
}

static void handle_speech_end(void)
{
    /* 立即停 mic 检测，保证所有出口都经 finish_turn 重启 wakeword */
    s_busy = true;
    esp_err_t err = wakeword_stop();
    if (err != ESP_OK) {
        finish_turn(SYS_VOICE_ERROR, "监听停止失败");
        return;
    }
    poll_cancel();
    if (s_cancel) {
        finish_turn(SYS_VOICE_IDLE, NULL);
        return;
    }

    size_t n = wakeword_get_utterance(s_utt_buf, MAX_UTTERANCE_SAMPLES);
    if (n == 0) {
        ESP_LOGW(TAG, "turn %" PRIu32 " 录音为空", s_turn_id);
        finish_turn(SYS_VOICE_IDLE, NULL);
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
        finish_turn(SYS_VOICE_ERROR, "录音失败");
        return;
    }
    ESP_LOGI(TAG, "turn %" PRIu32 " WAV %u样本 峰值%d", s_turn_id, (unsigned)n, (int)peak);

    char asr_text[ASR_TEXT_MAX];
    err = run_asr(asr_text, sizeof(asr_text));
    if (stage_failed(err, "识别失败")) return;
    if (asr_text[0] == '\0') {
        finish_turn(SYS_VOICE_ERROR, "未听清");
        return;
    }
    ESP_LOGI(TAG, "turn %" PRIu32 " ASR: %s", s_turn_id, asr_text);

    const char *reply = NULL;
    err = run_chat(asr_text, &reply);
    if (stage_failed(err, "回答失败")) return;

    err = run_tts(reply);
    if (stage_failed(err, "播放失败")) return;
    finish_turn(SYS_VOICE_IDLE, NULL);
}

static void dispatch_wake_event(wakeword_event_t evt)
{
    switch (evt) {
    case WAKEWORD_EVENT_WAKE:
        if (s_in_turn) {
            break; /* 上一轮未结束，忽略重复唤醒 */
        }
        s_in_turn = true;
        s_turn_id++;
        clear_cancel_flags();
        voice_alert(SYS_VOICE_LISTENING, NULL);
        ESP_LOGI(TAG, "turn %" PRIu32 " 唤醒", s_turn_id);
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
        vTaskDelete(NULL);
        return;
    }
    err = wakeword_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wakeword_start失败: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    voice_alert(SYS_VOICE_IDLE, NULL);
    ESP_LOGI(TAG, "就绪，等待唤醒词 | 栈余 %" PRIu32 "B",
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
