#pragma once

/*
 * voice_assistant 组件内部共享声明，仅供本组件内 .c 文件互相 include。
 * 不放进 include/，不对其他组件暴露——对外唯一入口是 voice_assistant.h
 * 的 voice_assistant_start()。文件按职责拆分只为压回 400 行/60 行红线，
 * 不构成新的组件边界。
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "sys_state.h"

/* ---- voice_assistant.c ↔ voice_text_turn.c 轮次共享原语 ---- */

/* 新一轮开始：占位防重入、发轮次号、清取消标志；返回新轮次号 */
uint32_t va_turn_next(void);
uint32_t va_turn_id(void);
volatile bool *va_turn_cancel_flag(void);

/* 检查阶段结果：取消→IDLE，错误→ERROR(带原因)。返回 true 表示 turn 已结束 */
bool va_stage_failed(esp_err_t err, const char *fail_reason);
void va_finish_turn(sys_voice_phase_t phase, const char *reason);

/* ---- voice_text_turn.c：agent 对话轮与串口文本注入测试轮 ---- */

/* 跑一轮 agent(工具循环+流式TTS)；*out_streamed 表示回答是否已流式播出 */
esp_err_t va_run_chat(const char *user_text, const char **reply, bool *out_streamed);

/* 非流式回答整段播报；流式已由 delta 回调播过。返回 true 表示本轮已结束 */
bool va_play_full_reply(const char *reply, bool streamed);

void va_handle_text_turn(const char *text);

/* asr_client 读取的固定录音路径，voice_assistant.c 和 voice_wav.c 共用 */
#define WAV_REL_PATH "va_utt.wav"

/* ---- voice_wav.c：录音后处理(裁剪静音+归一化)与落盘 ---- */
/* 就地裁剪 pcm 尾部静音(阈值取自信号自身峰值)，返回裁剪后样本数(<=n)。
 * VAD 静音容忍拉到 2000ms 后录音尾部常带静音，上传前裁掉能缩短ASR往返 */
size_t va_wav_trim_trailing_silence(int16_t *pcm, size_t n);

/* 就地做数字增益归一化(远场录音电平低)，返回归一化前的峰值供调用方记日志 */
int32_t va_wav_normalize_utterance(int16_t *pcm, size_t n);

/* 写入固定相对路径 WAV 文件，供 asr_client_transcribe() 读取 */
esp_err_t va_wav_save_utterance(const int16_t *pcm, size_t n);

/* ---- voice_tts_play.c：PCM 流播放会话(va_tts_play 与 reply 流式共用) ---- */

/* 创建流缓冲与信号量，voice_assistant_start() 里调用一次 */
esp_err_t va_tts_stream_init(void);

/* 合成 text 并边收边放；返回时音频已放完(或被 cancel_flag 提前中止)。
 * out_status 透传 HTTP 状态码(可传 NULL) */
esp_err_t va_tts_play(const char *text, const volatile bool *cancel_flag, int *out_status);

/* 过滤 Markdown 记号后再流式播放；过滤缓存分配失败时返回ESP_ERR_NO_MEM，
 * 绝不退回原文，以免把Markdown记号朗读出来。 */
esp_err_t va_tts_play_clean(const char *text, const volatile bool *cancel_flag, int *out_status);

/* 播放会话：tts_client 合成回调与启停，reply 流式任务复用同一播放任务 */
typedef struct {
    const volatile bool *cancel_flag;
    uint32_t gen; /* 合成发起时的播放代次快照，回调内校验防旧任务推入新轮流 */
} tts_play_ctx_t;
esp_err_t tts_play_synth_cb(const int16_t *samples, size_t sample_count, void *ctx);
esp_err_t tts_play_session_start(void);
esp_err_t tts_play_session_close(void);
void tts_play_session_reset(void);
uint32_t tts_play_session_gen(void); /* 当前播放代次，供合成回调 ctx 快照 */

/* ---- voice_tts_stream.c：TTS 流式合成+播放 ---- */


/* 将模型增量文本按句排队给 TTS；后台任务负责边接收边播放，调用方只传递片段。 */
esp_err_t va_reply_stream_start(const volatile bool *cancel_flag);

esp_err_t va_reply_stream_push_delta(const char *delta);
bool va_reply_stream_has_content(void);
esp_err_t va_reply_stream_finish(void);

/* ---- voice_reminder.c：提醒播报的音频链路独占 ---- */
typedef void (*va_reminder_transition_cb_t)(void);
void va_reminder_play(va_reminder_transition_cb_t on_started,
                      va_reminder_transition_cb_t on_finished,
                      const volatile bool *cancel_flag);
