#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ES7210(录音ADC) + ES8311(播放DAC) 统一I2S/I2C驱动，基于官方
 * esp_codec_dev 组件(ES7210录音only、ES8311播放+录音双模式，本项目只用其播放侧)。
 * 引脚见 board_rlcd42.h；I2C总线复用 board_i2c_bus()，
 * 不新建总线(架构规则：I2C总线唯一由board_rlcd42初始化)。
 *
 * 采样率固定16kHz mono 16-bit PCM，与P5.0已验证的ASR/TTS格式一致，
 * 上层(wakeword/asr_client/tts_client)不需要重采样。
 *
 * read/write 是阻塞调用，由内部I2S DMA驱动；cancel后正在阻塞的调用尽快
 * 返回ESP_ERR_NOT_FINISHED，用于半双工场景打断当前录音或播放。
 */

#define AUDIO_PIPELINE_SAMPLE_RATE 16000

esp_err_t audio_pipeline_init(void);

bool audio_pipeline_is_ready(void);

/* 阻塞读取count个int16 mono样本(来自ES7210单路mic，回采/AEC通道暂不暴露)。
 * 返回实际读到的样本数；超时或cancel后返回0，用 audio_pipeline_is_ready()
 * 区分是否因为设备未就绪 */
size_t audio_pipeline_read(int16_t *buf, size_t count, uint32_t timeout_ms);

/* 阻塞写入 count 个 int16 mono 样本到 ES8311 播放。不管理 PA_EN——流式播放场景
 * (多次调用写同一段音频)由调用方在会话首尾调用 audio_pipeline_set_output_enable()，
 * 避免每次调用都开关 PA 产生咔哒声。底层 codec 写接口不提供超时参数；需要中断时
 * 调用 audio_pipeline_cancel()，当前块结束后返回 ESP_ERR_NOT_FINISHED。 */
esp_err_t audio_pipeline_write(const int16_t *buf, size_t count);

/* 显式控制功放使能。播放前 enable(true)，整段(可能多次write)播放完 enable(false)。 */
void audio_pipeline_set_output_enable(bool enable);

/* 置位取消标志：正在阻塞的read/write尽快返回，不清空标志本身。
 * 调用方在开始下一次read/write前必须调用 audio_pipeline_clear_cancel() */
void audio_pipeline_cancel(void);

void audio_pipeline_clear_cancel(void);

#ifdef __cplusplus
}
#endif
