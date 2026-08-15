/*
 * voice_text_turn.c：agent 对话轮(va_run_chat 及流式回调)与串口文本注入
 * 测试轮。语音轮(handle_speech_end)与文本轮共用同一条 agent/TTS 链路，
 * 拆出本文件只为把 voice_assistant.c 压回 400 行红线。
 */
#include "voice_assistant_internal.h"
#include <inttypes.h>
#include "agent_runtime.h"
#include "esp_log.h"
#include "voice_phase.h"

static const char *TAG = "voice_assistant";

static void on_agent_tool_state(bool active, void *ctx)
{
    (void)ctx;
    /* 混合输出（先出文字再调工具）时播报已开始，工具状态不再影响 UI，
     * 否则 SPEAKING->EXECUTING/THINKING 被状态机拒绝刷警告 */
    if (voice_phase_get() == SYS_VOICE_SPEAKING) {
        return;
    }
    voice_alert(active ? SYS_VOICE_EXECUTING : SYS_VOICE_THINKING, NULL);
}

static esp_err_t s_reply_stream_err;

static void on_agent_reply_delta(const char *delta, void *ctx)
{
    (void)ctx;
    voice_alert(SYS_VOICE_SPEAKING, NULL);
    if (s_reply_stream_err == ESP_OK) {
        s_reply_stream_err = va_reply_stream_push_delta(delta);
    }
}

esp_err_t va_run_chat(const char *user_text, const char **reply, bool *out_streamed)
{
    s_reply_stream_err = va_reply_stream_start(va_turn_cancel_flag());
    if (s_reply_stream_err != ESP_OK) return s_reply_stream_err;
    voice_alert(SYS_VOICE_THINKING, NULL);
    agent_runtime_turn_options_t options = {
        .cancel_flag = va_turn_cancel_flag(),
        .tool_state_cb = on_agent_tool_state,
        .tool_state_ctx = NULL,
        .reply_delta_cb = on_agent_reply_delta,
        .reply_delta_ctx = NULL,
    };
    esp_err_t err = agent_runtime_run_turn(user_text, &options, reply);
    esp_err_t stream_err = va_reply_stream_finish();
    /* 45s总预算超时但已流出并播出部分内容：不再报"回答失败"覆盖已播报的语音——
     * *reply此时可能为NULL(run_loop超时时未走到finish_reply)，末尾日志判空处理。 */
    bool truncated = err == ESP_ERR_TIMEOUT && s_reply_stream_err == ESP_OK
                     && stream_err == ESP_OK && va_reply_stream_has_content();
    if (truncated) {
        ESP_LOGW(TAG, "turn %" PRIu32 " 回答被截断: 已播出部分内容", va_turn_id());
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "智能体失败: %s", esp_err_to_name(err));
        return err;
    } else if (s_reply_stream_err != ESP_OK || stream_err != ESP_OK) {
        return s_reply_stream_err != ESP_OK ? s_reply_stream_err : stream_err;
    }
    *out_streamed = truncated ? true : va_reply_stream_has_content();
    ESP_LOGI(TAG, "turn %" PRIu32 " LLM: %s", va_turn_id(), *reply ? *reply : "(截断)");
    return ESP_OK;
}

/* 非流式回答整段播报；流式已由 on_agent_reply_delta 逐句播过。
 * 返回 true 表示本轮已结束(出错或取消) */
bool va_play_full_reply(const char *reply, bool streamed)
{
    if (streamed) {
        return false;
    }
    if (!reply || !reply[0]) {
        va_finish_turn(SYS_VOICE_ERROR, "回答失败");
        return true;
    }
    voice_alert(SYS_VOICE_SPEAKING, NULL);
    esp_err_t err = va_tts_play_clean(reply, va_turn_cancel_flag(), NULL);
    return va_stage_failed(err, "播放失败");
}

/* 串口文本注入的测试轮：跳过录音/ASR，其余(agent工具循环/TTS播报/取消)
 * 与语音轮完全一致，供不依赖麦克风的链路验收 */
void va_handle_text_turn(const char *text)
{
    uint32_t turn = va_turn_next();
    ESP_LOGI(TAG, "turn %" PRIu32 " 文本注入: %s", turn, text);
    const char *reply = NULL;
    bool streamed = false;
    esp_err_t err = va_run_chat(text, &reply, &streamed);
    if (va_stage_failed(err, "回答失败")) {
        return;
    }
    if (va_play_full_reply(reply, streamed)) {
        return;
    }
    va_finish_turn(SYS_VOICE_IDLE, NULL);
}
