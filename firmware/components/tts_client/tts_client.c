/*
 * tts_client: SiliconFlow TTS HTTP流式对接
 *
 * 用cloud_transport_post_stream做HTTPS流式POST，响应体是纯PCM字节流(16kHz/mono/
 * 16-bit有符号小端，无WAV头)。cloud_transport按网络MTU分片，分片边界不保证落在
 * 2字节样本边界上，内部维护跨chunk的1字节残留缓冲——关键正确性点。
 */

#include "tts_client.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cloud_transport.h"
#include "esp_log.h"

static const char *TAG = "tts_client";

#define TTS_URL            "https://api.siliconflow.cn/v1/audio/speech"
#define MAX_API_KEY_LEN    256

static char s_api_key[MAX_API_KEY_LEN];   /* 初始化时写入，后续只读 */

/* PCM样本拼接上下文：cloud_transport分片不保证偶数长度，用pending_byte跨chunk拼接 */
struct pcm_ctx {
    bool has_pending;                     /* 是否有未配对的残留字节 */
    uint8_t pending_byte;                 /* 残留字节(低字节，等待下个chunk的首字节配对) */
    tts_client_pcm_cb_t user_on_pcm;      /* 调用方真正要通知的PCM回调 */
    void *user_ctx;                       /* 透传给user_on_pcm的ctx */
};

/*
 * cloud_transport分片回调：确保on_pcm永远只收到完整int16_t样本。
 *
 * PCM字节序为小端(ESP32也是小端)，能整除2的部分直接reinterpret成int16_t数组。
 * 上个chunk若为奇数长度，最后1字节暂存在pending_byte，等当前chunk的首字节拼成
 * 一个完整样本后再处理剩余数据。当前chunk末尾若有奇数残留也存下来等下次。
 */
static esp_err_t on_chunk(const char *data, size_t len, void *ctx)
{
    struct pcm_ctx *pc = (struct pcm_ctx *)ctx;
    size_t offset = 0;

    /* 前一个chunk的残留字节 + 当前chunk的第1字节 → 1个完整样本 */
    if (pc->has_pending && len > 0) {
        /* pending_byte是低字节，data[0]是高字节 → 拼成小端int16_t */
        int16_t sample = (int16_t)(((uint16_t)(uint8_t)data[0] << 8)
                                   | (uint16_t)pc->pending_byte);
        esp_err_t err = pc->user_on_pcm(&sample, 1, pc->user_ctx);
        if (err != ESP_OK) {
            return err;
        }
        offset = 1;
        pc->has_pending = false;
    }

    /* 剩余数据中能整除2的部分直接作为int16_t数组递回调 */
    size_t remaining = len - offset;
    size_t full_count = remaining / 2;
    if (full_count > 0) {
        const int16_t *samples = (const int16_t *)(data + offset);
        esp_err_t err = pc->user_on_pcm(samples, full_count, pc->user_ctx);
        if (err != ESP_OK) {
            return err;
        }
    }

    /* 奇数剩余：末尾1字节存为残留 */
    if (remaining & 1) {
        pc->pending_byte = (uint8_t)data[len - 1];
        pc->has_pending = true;
    }

    return ESP_OK;
}

/* 用cJSON构造SiliconFlow TTS请求体——input文本走cJSON序列化，天然转义JSON特殊字符 */
static char *build_request_body(const char *text)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "model", "FunAudioLLM/CosyVoice2-0.5B");
    cJSON_AddStringToObject(root, "input", text);
    cJSON_AddStringToObject(root, "voice", "FunAudioLLM/CosyVoice2-0.5B:anna");
    cJSON_AddStringToObject(root, "response_format", "pcm");
    cJSON_AddNumberToObject(root, "sample_rate", 16000);
    cJSON_AddBoolToObject(root, "stream", 1);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

esp_err_t tts_client_init(const char *api_key)
{
    if (!api_key) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(api_key);
    if (len >= MAX_API_KEY_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_api_key, api_key, len + 1);
    return ESP_OK;
}

esp_err_t tts_client_synthesize(const char *text, tts_client_pcm_cb_t on_pcm,
                                void *user_ctx, const volatile bool *cancel_flag,
                                int *out_status)
{
    if (!text || !on_pcm || !out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_api_key[0]) {
        return ESP_ERR_INVALID_ARG;   /* 未调用init */
    }

    *out_status = 0;

    /* 构造JSON请求体 */
    char *body = build_request_body(text);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    size_t body_len = strlen(body);

    /* 初始化跨chunk拼接上下文 */
    struct pcm_ctx pc = {
        .has_pending = false,
        .pending_byte = 0,
        .user_on_pcm = on_pcm,
        .user_ctx = user_ctx,
    };

    /* 首次请求 */
    esp_err_t err = cloud_transport_post_stream(
        TTS_URL, s_api_key, "application/json",
        body, body_len, on_chunk, &pc, cancel_flag, out_status);

    /* 429(限流)/503/504(服务暂时不可用)自动重试一次 */
    if (*out_status == 429 || *out_status == 503 || *out_status == 504) {
        ESP_LOGW(TAG, "HTTP %d, 自动重试一次", *out_status);
        pc.has_pending = false;     /* 重试是全新流，残留状态复位 */
        pc.pending_byte = 0;
        *out_status = 0;
        err = cloud_transport_post_stream(
            TTS_URL, s_api_key, "application/json",
            body, body_len, on_chunk, &pc, cancel_flag, out_status);
    }

    free(body);
    return err;
}
