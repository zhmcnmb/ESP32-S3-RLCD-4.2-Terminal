#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SiliconFlow ASR 客户端：上传WAV文件到 /v1/audio/transcriptions，
 * 返回识别文本。阻塞式，内部使用 cloud_transport_post_stream 做
 * multipart/form-data 上传，PSRAM 缓冲 WAV 和请求体。
 *
 * API 参考：https://docs.siliconflow.cn/api-reference/audio/audio-transcriptions
 */

/*
 * 初始化ASR客户端，保存API Key供后续 transcribe 调用。
 * api_key: SiliconFlow API Key (不持有"Bearer "前缀，纯key字符串)
 */
esp_err_t asr_client_init(const char *api_key);

/*
 * 阻塞：上传SD卡上的wav_rel_path(相对storage_sd挂载点的路径，
 * 如"agent/tmp/utterance.wav"，内容须是16kHz、mono、16-bit PCM WAV)，
 * 识别结果写入out_text(以'\0'结尾)。
 * cancel_flag可选，语义同cloud_transport_post_stream()。
 * *out_status: HTTP握手成功后写入实际状态码(2xx或401/429/503/504)，
 *              网络层失败时保持0。
 *
 * 失败返回:
 *   ESP_ERR_INVALID_ARG       参数为空/out_text_size太小，或未调用init
 *   ESP_ERR_NOT_FOUND         wav_rel_path读不到数据(文件不存在或SD未挂载)
 *   ESP_ERR_NO_MEM            内部缓冲分配失败
 *   ESP_ERR_NOT_FINISHED      cancel_flag被置位
 *   ESP_ERR_HTTP_CONNECT      HTTP状态码非2xx(*out_status带具体码)
 *   ESP_ERR_INVALID_RESPONSE  响应JSON里没有text字段
 *   其余cloud_transport_post_stream()透传的网络/TLS错误
 */
esp_err_t asr_client_transcribe(const char *wav_rel_path, char *out_text, size_t out_text_size,
                                const volatile bool *cancel_flag, int *out_status);

#ifdef __cplusplus
}
#endif
