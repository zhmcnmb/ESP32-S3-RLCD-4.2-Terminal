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

/* ---- voice_tts_stream.c：TTS 流式合成+播放 ---- */

/* 创建流缓冲与信号量，voice_assistant_start() 里调用一次 */
esp_err_t va_tts_stream_init(void);

/* 合成 text 并边收边放；返回时音频已放完(或被 cancel_flag 提前中止)。
 * out_status 透传 HTTP 状态码(可传 NULL) */
esp_err_t va_tts_play(const char *text, const volatile bool *cancel_flag, int *out_status);
