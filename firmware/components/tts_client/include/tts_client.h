#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * TTS客户端: 对接 SiliconFlow TTS API (`POST /v1/audio/speech`)，流式接收
 * 原始PCM(16kHz/mono/16-bit有符号小端)，逐片段回调给调用方。
 *
 * 底层使用持久连接(cloud_transport_conn_*)做HTTPS流式传输，一次回答的多个合成块
 * 复用同一条TLS连接省去句间握手；响应体是纯PCM字节流
 * (无WAV容器头)，cloud_transport按网络MTU分片，分片边界不保证落在2字节样本边界上，
 * tts_client内部维护跨chunk的1字节残留缓冲，保证回调永远只收到完整int16_t样本。
 *
 * 依赖 net_manager 已联网；调用方无WiFi时自行安排降级策略。
 */

/* PCM样本回调：每次收到一段完整对齐的16-bit有符号小端样本时调用。
 * samples指向int16_t数组，sample_count是样本个数(不是字节数)。
 * 返回非ESP_OK会中止合成并透传该错误码。 */
typedef esp_err_t (*tts_client_pcm_cb_t)(const int16_t *samples, size_t sample_count, void *ctx);

/* 初始化TTS客户端：全局只需调用一次，api_key是SiliconFlow平台API密钥。
 * 必须在第一次调用tts_client_synthesize()之前调用。 */
esp_err_t tts_client_init(const char *api_key);

/* 阻塞请求TTS并流式回调PCM样本给on_pcm。
 *
 * text: 要合成的文本(UTF-8)。内部用cJSON序列化到请求体JSON的input字段，
 *       天然转义文本中的引号/换行等特殊字符。
 * on_pcm: 每次收到一组完整int16_t样本时调用的回调。
 * user_ctx: 透传给on_pcm的上下文指针。
 * cancel_flag: 可选(可传NULL)。传输会在请求分片和响应读取间检查；底层单次I/O结束后
 *              才可中止并返回ESP_ERR_NOT_FINISHED。
 * out_status: 成功取得响应头后写入HTTP状态码；连接/TLS失败时为0。取消或超时若发生在
 *             响应头之后，该值可能已写入，调用方应优先判断返回错误码。
 *
 * 失败返回:
 *   ESP_ERR_INVALID_ARG      参数为空或未调用tts_client_init()
 *   ESP_ERR_NO_MEM           JSON请求体分配失败
 *   ESP_ERR_NOT_FINISHED     cancel_flag被置位
 *   ESP_ERR_INVALID_STATE    上一请求仍在使用持久连接；本次不会干扰它
 *   ESP_ERR_HTTP_CONNECT     HTTP状态非2xx(*out_status带具体码)
 *   ESP_ERR_INVALID_RESPONSE PCM为空、不是完整16-bit样本或on_pcm回调中止传输
 *   其余cloud_transport连接路径透传的网络/TLS错误
 */
esp_err_t tts_client_synthesize(const char *text, tts_client_pcm_cb_t on_pcm, void *user_ctx,
                                const volatile bool *cancel_flag, int *out_status);

#ifdef __cplusplus
}
#endif
