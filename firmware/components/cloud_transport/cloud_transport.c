#include "cloud_transport.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "miniz.h"

static const char *TAG = "cloud_transport";

#define RAW_BUF_SIZE 1024 /* gzip压缩体上限，够用QWeather这类小JSON响应；未来大响应场景再评估流式/PSRAM */
#define HTTP_TIMEOUT_MS 8000

typedef struct {
    char *buf;
    int len;
    int cap;
    bool gzip;
    bool truncated; /* 响应超RAW_BUF_SIZE时置位 */
} fetch_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    fetch_ctx_t *ctx = (fetch_ctx_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (strcasecmp(evt->header_key, "Content-Encoding") == 0 &&
            strcasecmp(evt->header_value, "gzip") == 0) {
            ctx->gzip = true;
        }
        break;
    case HTTP_EVENT_ON_DATA: {
        int remain = ctx->cap - ctx->len;
        if (remain <= 0) {
            ctx->truncated = true;
            break;
        }
        int n = evt->data_len < remain ? evt->data_len : remain;
        memcpy(ctx->buf + ctx->len, evt->data, n);
        ctx->len += n;
        if (n < evt->data_len) {
            ctx->truncated = true;
        }
        break;
    }
    default:
        break;
    }
    return ESP_OK;
}

/* 剥离gzip容器头(RFC1952)，返回DEFLATE数据起始偏移；格式不合法返回负值 */
static int gzip_skip_header(const uint8_t *buf, int len)
{
    if (len < 10 || buf[0] != 0x1f || buf[1] != 0x8b || buf[2] != 8 /* CM=deflate */) {
        return -1;
    }
    uint8_t flg = buf[3];
    int pos = 10;
    if (flg & 0x04) { /* FEXTRA */
        if (pos + 2 > len) {
            return -1;
        }
        int xlen = buf[pos] | (buf[pos + 1] << 8);
        pos += 2 + xlen;
    }
    if (flg & 0x08) { /* FNAME，以'\0'结尾的文件名 */
        while (pos < len && buf[pos] != 0) {
            pos++;
        }
        pos++;
    }
    if (flg & 0x10) { /* FCOMMENT，以'\0'结尾的注释 */
        while (pos < len && buf[pos] != 0) {
            pos++;
        }
        pos++;
    }
    if (flg & 0x02) { /* FHCRC */
        pos += 2;
    }
    return (pos < len) ? pos : -1;
}

static esp_err_t perform_get(const char *url, const char *header_name, const char *header_value,
                             fetch_ctx_t *ctx)
{
    esp_http_client_config_t cfg = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .event_handler = http_event_handler,
        .user_data = ctx, .timeout_ms = HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    if (header_name && header_value) {
        esp_err_t set_err = esp_http_client_set_header(client, header_name, header_value);
        if (set_err != ESP_OK) {
            esp_http_client_cleanup(client);
            return set_err;
        }
    }
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK && ctx->truncated) {
        err = ESP_ERR_INVALID_SIZE;
    }
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "请求失败: %s", esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP状态非2xx: %d", status);
        return ESP_ERR_HTTP_CONNECT;
    }
    return ESP_OK;
}

static esp_err_t decompress_gzip(const fetch_ctx_t *ctx, char *out_buf, size_t out_buf_size,
                                 size_t *out_len)
{
    int deflate_start = gzip_skip_header((const uint8_t *)ctx->buf, ctx->len);
    int deflate_len = deflate_start >= 0 ? ctx->len - deflate_start - 8 : -1;
    if (deflate_start < 0 || deflate_len <= 0) {
        ESP_LOGW(TAG, "gzip容器头解析失败, raw_len=%d", ctx->len);
        return ESP_ERR_INVALID_RESPONSE;
    }
    /* tinfl_decompressor约11KB，便捷接口会把它放到调用者栈上，曾打爆
     * weather_task的4KB栈；改用底层接口并放PSRAM，避免耗掉内部堆。 */
    tinfl_decompressor *decomp = heap_caps_calloc(1, sizeof(*decomp), MALLOC_CAP_SPIRAM);
    if (!decomp) return ESP_ERR_NO_MEM;
    tinfl_init(decomp);
    size_t in_len = (size_t)deflate_len;
    size_t decompressed = out_buf_size - 1;
    tinfl_status status = tinfl_decompress(decomp, (const mz_uint8 *)(ctx->buf + deflate_start), &in_len,
                                           (mz_uint8 *)out_buf, (mz_uint8 *)out_buf, &decompressed,
                                           TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    heap_caps_free(decomp);
    if (status != TINFL_STATUS_DONE) {
        ESP_LOGW(TAG, "gzip解压失败(status=%d，可能是output缓冲区不够大)", (int)status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    out_buf[decompressed] = '\0';
    *out_len = decompressed;
    return ESP_OK;
}

static esp_err_t copy_response(const fetch_ctx_t *ctx, char *out_buf, size_t out_buf_size,
                               size_t *out_len)
{
    if (ctx->gzip) return decompress_gzip(ctx, out_buf, out_buf_size, out_len);
    size_t len = (size_t)ctx->len < out_buf_size - 1 ? (size_t)ctx->len : out_buf_size - 1;
    memcpy(out_buf, ctx->buf, len);
    out_buf[len] = '\0';
    *out_len = len;
    return ESP_OK;
}

esp_err_t cloud_transport_get(cloud_transport_get_request_t *request)
{
    if (!request || !request->url || !request->response || request->response_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    char *raw_buf = malloc(RAW_BUF_SIZE);
    if (!raw_buf) return ESP_ERR_NO_MEM;
    fetch_ctx_t ctx = { .buf = raw_buf, .len = 0, .cap = RAW_BUF_SIZE, .gzip = false };
    esp_err_t err = perform_get(request->url, request->header_name, request->header_value, &ctx);
    if (err == ESP_OK) {
        err = copy_response(&ctx, request->response, request->response_size, &request->out_len);
    }
    free(raw_buf);
    return err;
}
