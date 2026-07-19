#include "asr_client.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cloud_transport.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "storage_sd.h"

static const char *TAG = "asr_client";

/* ---------- 常量 ---------- */

#define BOUNDARY        "----ESP32AsrBoundary7MA4YWxk"
#define ASR_URL         "https://api.siliconflow.cn/v1/audio/transcriptions"
#define MAX_WAV_SIZE    (512 * 1024)        /* 15秒16kHz mono 16-bit PCM约480KB */
#define RESP_BUF_SIZE   1024                /* 响应JSON很小，1KB足够 */
#define READ_CHUNK_SIZE 4096                /* SD卡分块读取大小 */

/*
 * multipart body格式(严格匹配SiliconFlow要求)：
 * --BOUNDARY\r\nContent-Disposition: form-data; name="model"\r\n\r\n
 * FunAudioLLM/SenseVoiceSmall\r\n--BOUNDARY\r\nContent-Disposition: form-data;
 * name="file"; filename="utterance.wav"\r\nContent-Type: audio/wav\r\n\r\n
 * <WAV二进制>\r\n--BOUNDARY--\r\n
 */
static const char PREAMBLE[] =
    "--" BOUNDARY "\r\n"
    "Content-Disposition: form-data; name=\"model\"\r\n"
    "\r\n"
    "FunAudioLLM/SenseVoiceSmall\r\n"
    "--" BOUNDARY "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"utterance.wav\"\r\n"
    "Content-Type: audio/wav\r\n"
    "\r\n";

static const char POSTAMBLE[] = "\r\n--" BOUNDARY "--\r\n";

#define PRE_SIZE  (sizeof(PREAMBLE) - 1)   /* 不计结尾'\0' */
#define POST_SIZE (sizeof(POSTAMBLE) - 1)

/* ---------- 状态 ---------- */

static char s_api_key[128];   /* SiliconFlow API Key */
static bool s_initialized = false;

/* ---------- 响应收集上下文 ---------- */

typedef struct {
    char   *buf;   /* PSRAM缓冲 */
    size_t  cap;   /* buf容量（不含结尾'\0'的空间） */
    size_t  len;   /* 已写入字节数 */
} collect_ctx_t;

/* 每收到一块响应体就追加入ctx->buf */
static esp_err_t on_response_chunk(const char *data, size_t len, void *ctx)
{
    collect_ctx_t *c = (collect_ctx_t *)ctx;
    if (c->len + len > c->cap) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    memcpy(c->buf + c->len, data, len);
    c->len += len;
    return ESP_OK;
}

/* ---------- 读取WAV到PSRAM ---------- */

static esp_err_t read_wav_to_psram(const char *rel_path,
                                   uint8_t **out_buf, size_t *out_len)
{
    if (!rel_path || !out_buf || !out_len) return ESP_ERR_INVALID_ARG;
    uint32_t file_size = 0;
    esp_err_t size_err = storage_sd_get_file_size(rel_path, &file_size);
    if (size_err != ESP_OK) return size_err;
    if (file_size == 0 || file_size > MAX_WAV_SIZE) return ESP_ERR_INVALID_SIZE;
    uint8_t *buf = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!buf) return ESP_ERR_NO_MEM;

    size_t total = 0;
    while (total < file_size) {
        size_t want = file_size - total;
        if (want > READ_CHUNK_SIZE) want = READ_CHUNK_SIZE;
        int n = storage_sd_read_chunk(rel_path, (uint32_t)total, buf + total, want);
        if (n <= 0) {
            heap_caps_free(buf);
            return ESP_FAIL;
        }
        total += (size_t)n;
    }
    *out_buf = buf;
    *out_len = total;
    return ESP_OK;
}

/* ---------- 发送请求、收集响应、解析JSON ---------- */

/* 模型文本超过调用方缓冲时，不能在UTF-8连续字节中间截断。 */
static size_t fit_utf8_prefix(const char *text, size_t len, size_t out_size)
{
    if (len < out_size) return len;
    size_t keep = out_size - 1;
    while (keep > 0 && (((unsigned char)text[keep] & 0xc0) == 0x80)) {
        keep--;
    }
    return keep;
}

static esp_err_t parse_response(char *response, char *out_text, size_t out_text_size)
{
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        ESP_LOGE(TAG, "response JSON parse failed");
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *text_item = cJSON_GetObjectItem(root, "text");
    if (!text_item || !cJSON_IsString(text_item)) {
        ESP_LOGE(TAG, "response missing \"text\" field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t text_len = fit_utf8_prefix(text_item->valuestring, strlen(text_item->valuestring),
                                      out_text_size);
    memcpy(out_text, text_item->valuestring, text_len);
    out_text[text_len] = '\0';
    ESP_LOGI(TAG, "识别文本(%dB): '%s'", (int)text_len, out_text);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t send_and_parse(const uint8_t *body, size_t body_len,
                                const volatile bool *cancel_flag, int *out_status,
                                char *out_text, size_t out_text_size)
{
    /* 申请响应收集缓冲（多1字节给'\0'） */
    collect_ctx_t ctx;
    ctx.buf = heap_caps_malloc(RESP_BUF_SIZE + 1, MALLOC_CAP_SPIRAM);
    if (!ctx.buf) {
        return ESP_ERR_NO_MEM;
    }
    ctx.cap = RESP_BUF_SIZE;   /* 纯数据最多RESP_BUF_SIZE字节，下标0..RESP_BUF_SIZE-1 */
    ctx.len = 0;

    const char *content_type = "multipart/form-data; boundary=" BOUNDARY;

    esp_err_t err = cloud_transport_post_stream(
        ASR_URL, s_api_key, content_type,
        body, body_len,
        on_response_chunk, &ctx,
        cancel_flag, out_status);

    /* 重试条件放宽为"非成功且非用户取消"：既覆盖429/503/504等HTTP错误状态，也覆盖
     * 网络层失败(如观测到的ESP_FAIL/status=0 15s挂死)——WAV已在SD，重试只是重新拼
     * multipart上传，无副作用；ESP_ERR_NOT_FINISHED是用户主动取消，不重试。 */
    if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
        ESP_LOGW(TAG, "转写请求失败(%s, status=%d), retrying once", esp_err_to_name(err),
                 *out_status);
        ctx.len = 0;
        *out_status = 0;
        err = cloud_transport_post_stream(
            ASR_URL, s_api_key, content_type,
            body, body_len,
            on_response_chunk, &ctx,
            cancel_flag, out_status);
    }

    if (err != ESP_OK) {
        heap_caps_free(ctx.buf);
        return err;
    }

    /* 截断到实际有效数据长度，加'\0'后交给JSON解析。 */
    ctx.buf[ctx.len] = '\0';
    err = parse_response(ctx.buf, out_text, out_text_size);
    heap_caps_free(ctx.buf);
    return err;
}

/* ========== 公开接口 ========== */

esp_err_t asr_client_init(const char *api_key)
{
    if (!api_key || strlen(api_key) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t key_len = strlen(api_key);
    if (key_len >= sizeof(s_api_key)) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(s_api_key, api_key, key_len + 1);   /* 含'\0' */
    s_initialized = true;

    ESP_LOGI(TAG, "ASR client initialized");
    return ESP_OK;
}

esp_err_t asr_client_transcribe(const char *wav_rel_path, char *out_text,
                                size_t out_text_size,
                                const volatile bool *cancel_flag, int *out_status)
{
    if (!s_initialized || !wav_rel_path || !out_text || out_text_size == 0 || !out_status) {
        if (out_status) *out_status = 0;
        return ESP_ERR_INVALID_ARG;
    }
    if (out_status) *out_status = 0;

    /* SD卡检查 */
    if (!storage_sd_is_mounted()) {
        return ESP_ERR_NOT_FOUND;
    }

    /* 1. 读WAV文件到PSRAM */
    uint8_t *wav_buf = NULL;
    size_t   wav_len = 0;
    esp_err_t err = read_wav_to_psram(wav_rel_path, &wav_buf, &wav_len);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "读到WAV %uB", (unsigned)wav_len);

    /* 2. 构造完整multipart body */
    size_t  body_len = PRE_SIZE + wav_len + POST_SIZE;
    uint8_t *body = heap_caps_malloc(body_len, MALLOC_CAP_SPIRAM);
    if (!body) {
        heap_caps_free(wav_buf);
        return ESP_ERR_NO_MEM;
    }

    memcpy(body,                   PREAMBLE,  PRE_SIZE);
    memcpy(body + PRE_SIZE,        wav_buf,   wav_len);
    memcpy(body + PRE_SIZE + wav_len, POSTAMBLE, POST_SIZE);

    heap_caps_free(wav_buf);   /* WAV数据已拷入body，释放 */

    /* 3. 发送请求+收集响应+解析JSON */
    ESP_LOGI(TAG, "上传 multipart body %uB", (unsigned)body_len);
    err = send_and_parse(body, body_len, cancel_flag, out_status,
                         out_text, out_text_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "转写请求失败: %s status=%d", esp_err_to_name(err),
                 out_status ? *out_status : -1);
    }

    heap_caps_free(body);
    return err;
}
