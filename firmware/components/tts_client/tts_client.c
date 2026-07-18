/*
 * tts_client: SiliconFlow TTS HTTP流式对接
 *
 * 用cloud_transport持久连接(cloud_transport_conn_*)做HTTPS流式POST，响应体是纯PCM
 * 字节流(16kHz/mono/16-bit有符号小端，无WAV头)。一次回答的多个合成块复用同一条
 * TLS连接，省去句间重复握手；cloud_transport按网络MTU分片，分片边界不保证落在
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
static cloud_stream_conn_t *s_conn;       /* 持久TLS连接：跨多次合成块复用，省句间握手 */

/* PCM样本拼接上下文：cloud_transport分片不保证偶数长度，用pending_byte跨chunk拼接 */
struct pcm_ctx {
    bool has_pending;                     /* 是否有未配对的残留字节 */
    uint8_t pending_byte;                 /* 残留字节(低字节，等待下个chunk的首字节配对) */
    bool got_data;                        /* 本次请求是否收到过任何字节(判断陈旧keep-alive连接) */
    tts_client_pcm_cb_t user_on_pcm;      /* 调用方真正要通知的PCM回调 */
    void *user_ctx;                       /* 透传给user_on_pcm的ctx */
};

/* 一次请求的不可变参数收进结构体，重试辅助函数不再扩展公开函数参数。 */
struct tts_request {
    const char *body;
    size_t body_len;
    tts_client_pcm_cb_t on_pcm;
    void *user_ctx;
    const volatile bool *cancel_flag;
    int *status;
};

/*
 * cloud_transport分片回调：确保on_pcm永远只收到完整int16_t样本。
 *
 * PCM字节序为小端(ESP32也是小端)，能整除2的部分直接reinterpret成int16_t数组。
 * 上个chunk若为奇数长度，最后1字节暂存在pending_byte，等当前chunk的首字节拼成
 * 一个完整样本后再处理剩余数据。当前chunk末尾若有奇数残留也存下来等下次。
 */
/* 前一片留下奇数尾字节时，下一片从第1字节后开始，地址不再满足int16_t对齐。
 * 分批解码到栈上的对齐数组，避免把未对齐地址传给音频回调。 */
static esp_err_t deliver_unaligned_samples(const char *data, size_t byte_len, struct pcm_ctx *pc)
{
    int16_t samples[128];
    size_t offset = 0;
    while (offset < byte_len) {
        size_t count = (byte_len - offset) / sizeof(int16_t);
        if (count > sizeof(samples) / sizeof(samples[0])) {
            count = sizeof(samples) / sizeof(samples[0]);
        }
        for (size_t i = 0; i < count; i++) {
            uint16_t raw = (uint8_t)data[offset + i * 2]
                         | ((uint16_t)(uint8_t)data[offset + i * 2 + 1] << 8);
            samples[i] = (int16_t)raw;
        }
        esp_err_t err = pc->user_on_pcm(samples, count, pc->user_ctx);
        if (err != ESP_OK) return err;
        offset += count * sizeof(int16_t);
    }
    return ESP_OK;
}

static esp_err_t on_chunk(const char *data, size_t len, void *ctx)
{
    struct pcm_ctx *pc = (struct pcm_ctx *)ctx;
    size_t offset = 0;
    if (len > 0) pc->got_data = true;

    /* 前一个chunk的残留字节 + 当前chunk的第1字节 → 1个完整样本 */
    if (pc->has_pending && len > 0) {
        /* pending_byte是低字节，data[0]是高字节 → 拼成小端int16_t */
        int16_t sample = (int16_t)(((uint16_t)(uint8_t)data[0] << 8)
                                   | (uint16_t)pc->pending_byte);
        esp_err_t err = pc->user_on_pcm(&sample, 1, pc->user_ctx);
        if (err != ESP_OK) return err;
        offset = 1;
        pc->has_pending = false;
    }

    size_t remaining = len - offset;
    size_t full_bytes = remaining & ~(sizeof(int16_t) - 1);
    if (full_bytes > 0) {
        esp_err_t err;
        if (offset == 0) {
            const int16_t *samples = (const int16_t *)data;
            err = pc->user_on_pcm(samples, full_bytes / sizeof(int16_t), pc->user_ctx);
        } else {
            err = deliver_unaligned_samples(data + offset, full_bytes, pc);
        }
        if (err != ESP_OK) return err;
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

    bool added = cJSON_AddStringToObject(root, "model", "FunAudioLLM/CosyVoice2-0.5B")
                 && cJSON_AddStringToObject(root, "input", text)
                 /* Jarvis默认使用官方预置沉稳男声；模型前缀是平台必填格式。 */
                 && cJSON_AddStringToObject(root, "voice", "FunAudioLLM/CosyVoice2-0.5B:alex")
                 && cJSON_AddStringToObject(root, "response_format", "pcm")
                 && cJSON_AddNumberToObject(root, "sample_rate", 16000)
                 && cJSON_AddBoolToObject(root, "stream", true);
    if (!added) {
        cJSON_Delete(root);
        return NULL;
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static void reset_pcm_ctx(struct pcm_ctx *pc, const struct tts_request *request)
{
    pc->has_pending = false;
    pc->pending_byte = 0;
    pc->got_data = false;
    pc->user_on_pcm = request->on_pcm;
    pc->user_ctx = request->user_ctx;
}

static esp_err_t open_connection(void)
{
    if (s_conn) return ESP_OK;
    esp_err_t err = cloud_transport_conn_open(&s_conn, TTS_URL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "建立持久连接失败: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t post_request(const struct tts_request *request, struct pcm_ctx *pc)
{
    return cloud_transport_conn_post_stream(s_conn, TTS_URL, s_api_key, "application/json",
                                            request->body, request->body_len, on_chunk, pc,
                                            request->cancel_flag, request->status);
}

static esp_err_t reconnect_and_post(const struct tts_request *request, struct pcm_ctx *pc)
{
    cloud_transport_conn_close(s_conn);
    s_conn = NULL;
    esp_err_t err = open_connection();
    if (err == ESP_OK) {
        reset_pcm_ctx(pc, request);
        *request->status = 0;
        err = post_request(request, pc);
    }
    return err;
}

static bool should_retry_empty(esp_err_t err, int status, bool got_data)
{
    return !got_data && (err == ESP_OK || (err != ESP_ERR_NOT_FINISHED
            && err != ESP_ERR_INVALID_STATE && status == 0));
}

static esp_err_t retry_empty_response(const struct tts_request *request, struct pcm_ctx *pc,
                                      esp_err_t err)
{
    if (!should_retry_empty(err, *request->status, pc->got_data)) return err;
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "服务端返回空PCM，重连重试一次");
    } else {
        ESP_LOGW(TAG, "疑似连接失效(%s)，重连重试一次", esp_err_to_name(err));
    }
    return reconnect_and_post(request, pc);
}

static bool is_temporary_status(int status)
{
    return status == 429 || status == 503 || status == 504;
}

static esp_err_t retry_temporary_status(const struct tts_request *request, struct pcm_ctx *pc,
                                        esp_err_t err)
{
    if (!is_temporary_status(*request->status) || err == ESP_ERR_NOT_FINISHED
        || err == ESP_ERR_INVALID_STATE) {
        return err;
    }
    ESP_LOGW(TAG, "HTTP %d，自动重试一次", *request->status);
    /* 非2xx响应理论上已被传输层读尽；仍重建一次，避免异常断流遗留半开连接。 */
    return reconnect_and_post(request, pc);
}

static esp_err_t validate_pcm_result(esp_err_t err, const struct pcm_ctx *pc)
{
    if (err != ESP_OK) return err;
    if (!pc->got_data) {
        ESP_LOGW(TAG, "TTS重试后仍返回空PCM");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (pc->has_pending) {
        ESP_LOGW(TAG, "PCM响应长度不是16-bit样本的整数倍");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static void close_failed_connection(esp_err_t err)
{
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        cloud_transport_conn_close(s_conn);
        s_conn = NULL;
    }
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
    if (!text || !on_pcm || !out_status || !s_api_key[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_status = 0;
    char *body = build_request_body(text);
    if (!body) return ESP_ERR_NO_MEM;
    struct tts_request request = {
        .body = body, .body_len = strlen(body), .on_pcm = on_pcm,
        .user_ctx = user_ctx, .cancel_flag = cancel_flag, .status = out_status,
    };
    struct pcm_ctx pc;
    reset_pcm_ctx(&pc, &request);
    esp_err_t err = open_connection();
    if (err == ESP_OK) {
        err = post_request(&request, &pc);
        err = retry_empty_response(&request, &pc, err);
        err = retry_temporary_status(&request, &pc, err);
        err = validate_pcm_result(err, &pc);
        close_failed_connection(err);
    }
    free(body);
    return err;
}
