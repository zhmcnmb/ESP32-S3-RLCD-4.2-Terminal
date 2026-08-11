/* cloud_transport_stream.c - 有取消、总超时和响应预算的 HTTPS POST。
 * 单次请求执行核心(cloud_transport_exec_request)供本文件的一次性路径
 * (post_stream)和 cloud_transport_conn.c 的持久连接路径共用。 */
#include "cloud_transport.h"
#include "cloud_transport_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "cloud_transport";
/* 目前所有云 Adapter 的 API Key 上限为 255B；另留 "Bearer " 前缀及结尾 NUL。 */
#define STREAM_AUTH_HEADER_SIZE (sizeof("Bearer ") + 256)


static esp_err_t stream_abort_reason(const volatile bool *cancel_flag, int64_t start_us)
{
    if ((esp_timer_get_time() - start_us) / 1000 >= STREAM_TOTAL_TIMEOUT_MS) {
        return ESP_ERR_TIMEOUT;
    }
    return cancel_flag && *cancel_flag ? ESP_ERR_NOT_FINISHED : ESP_OK;
}

static esp_err_t stream_prepare_response(esp_http_client_handle_t client,
                                         const volatile bool *cancel_flag, int64_t start_us,
                                         int *out_status)
{
    esp_err_t err = stream_abort_reason(cancel_flag, start_us);
    if (err != ESP_OK) return err;
    if (esp_http_client_fetch_headers(client) < 0) {
        return ESP_FAIL;
    }
    int status = esp_http_client_get_status_code(client);
    if (out_status) *out_status = status;
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP状态非2xx: %d", status);
        return ESP_ERR_HTTP_CONNECT;
    }
    return stream_abort_reason(cancel_flag, start_us);
}

static esp_err_t stream_read_loop(esp_http_client_handle_t client, cloud_transport_chunk_cb_t on_chunk,
                                  void *user_ctx, const volatile bool *cancel_flag, int64_t start_us)
{
    char *buf = malloc(STREAM_READ_CHUNK);
    if (!buf) return ESP_ERR_NO_MEM;
    esp_err_t result = ESP_OK;
    int eagain_count = 0;
    while (true) {
        result = stream_abort_reason(cancel_flag, start_us);
        if (result != ESP_OK) break;
        int n = esp_http_client_read(client, buf, STREAM_READ_CHUNK);
        if (n < 0) {
            if (n == -ESP_ERR_HTTP_EAGAIN && ++eagain_count <= 3) continue;
            result = n == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
            break;
        }
        if (n == 0) {
            result = esp_http_client_is_complete_data_received(client) ? ESP_OK : ESP_FAIL;
            break;
        }
        eagain_count = 0;
        result = on_chunk(buf, (size_t)n, user_ctx);
        if (result != ESP_OK || esp_http_client_is_complete_data_received(client)) break;
    }
    free(buf);
    return result;
}

/* 非2xx响应也必须读到结尾，才能安全复用服务端声明 keep-alive 的连接。 */
static esp_err_t discard_chunk(const char *data, size_t len, void *ctx)
{
    (void)data;
    (void)len;
    (void)ctx;
    return ESP_OK;
}

esp_err_t cloud_transport_client_create(const char *url, esp_http_client_handle_t *out_client)
{
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_POST, .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = STREAM_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    *out_client = client;
    return ESP_OK;
}

static esp_err_t stream_set_request_fields(esp_http_client_handle_t client, const char *url,
                                           const char *auth, const char *content_type,
                                           const char *accept)
{
    char auth_buf[STREAM_AUTH_HEADER_SIZE];
    if (auth) {
        int n = snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", auth);
        if (n < 0 || (size_t)n >= sizeof(auth_buf)) return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_http_client_set_url(client, url);
    if (err == ESP_OK && auth) {
        err = esp_http_client_set_header(client, "Authorization", auth_buf);
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_header(client, "Content-Type", content_type);
    }
    if (err == ESP_OK && accept) {
        err = esp_http_client_set_header(client, "Accept", accept);
    }
    return err;
}

esp_err_t cloud_transport_exec_request(esp_http_client_handle_t client, const char *url,
                                       const char *auth, const char *content_type,
                                       const char *accept, const void *body, size_t body_len,
                                       cloud_transport_chunk_cb_t on_chunk, void *user_ctx,
                                       const volatile bool *cancel_flag, int *out_status)
{
    if (!client || !url || !on_chunk || (body_len > 0 && !body)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (body_len > INT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    int64_t start_us = esp_timer_get_time();
    esp_err_t err = stream_set_request_fields(client, url, auth, content_type, accept);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "设置请求字段失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_http_client_open(client, (int)body_len);
    if (err == ESP_OK && body && body_len > 0) {
        const char *data = body;
        for (size_t off = 0; off < body_len;) {
            size_t chunk = body_len - off;
            if (chunk > STREAM_WRITE_CHUNK) chunk = STREAM_WRITE_CHUNK;
            err = stream_abort_reason(cancel_flag, start_us);
            if (err != ESP_OK) break;
            int written = esp_http_client_write(client, data + off, (int)chunk);
            if (written <= 0) {
                ESP_LOGW(TAG, "写请求体失败: %d @%u/%u", written, (unsigned)off,
                         (unsigned)body_len);
                err = ESP_ERR_HTTP_WRITE_DATA;
                break;
            }
            off += (size_t)written;
        }
    }
    if (err != ESP_OK) {
        if (err != ESP_ERR_NOT_FINISHED) {
            ESP_LOGW(TAG, "打开或写入失败: %s", esp_err_to_name(err));
        }
        return err;
    }
    err = stream_prepare_response(client, cancel_flag, start_us, out_status);
    if (err == ESP_ERR_HTTP_CONNECT) {
        esp_err_t drain_err = stream_read_loop(client, discard_chunk, NULL, cancel_flag, start_us);
        return drain_err == ESP_OK ? err : drain_err;
    }
    if (err == ESP_OK) err = stream_read_loop(client, on_chunk, user_ctx, cancel_flag, start_us);
    return err;
}

static esp_err_t run_stream_request(const char *url, const char *auth, const char *content_type,
                                    const char *accept, const void *body, size_t body_len,
                                    cloud_transport_chunk_cb_t on_chunk, void *user_ctx,
                                    const volatile bool *cancel_flag, int *out_status)
{
    esp_http_client_handle_t client = NULL;
    esp_err_t err = cloud_transport_client_create(url, &client);
    if (err == ESP_OK) {
        err = cloud_transport_exec_request(client, url, auth, content_type, accept, body, body_len,
                                           on_chunk, user_ctx, cancel_flag, out_status);
    }
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    return err;
}

esp_err_t cloud_transport_post_stream(const char *url, const char *auth_header_value,
                                      const char *content_type, const void *body, size_t body_len,
                                      cloud_transport_chunk_cb_t on_chunk, void *user_ctx,
                                      const volatile bool *cancel_flag, int *out_status)
{
    if (!url || !on_chunk) return ESP_ERR_INVALID_ARG;
    if (out_status) *out_status = 0;
    return run_stream_request(url, auth_header_value, content_type, NULL, body, body_len,
                              on_chunk, user_ctx, cancel_flag, out_status);
}

typedef struct {
    char location[512]; /* 本轮响应的 Location 头，空串=无 */
} redirect_ctx_t;

static esp_err_t redirect_event_handler(esp_http_client_event_t *evt)
{
    redirect_ctx_t *ctx = evt->user_data;
    if (ctx && evt->event_id == HTTP_EVENT_ON_HEADER
        && strcasecmp(evt->header_key, "Location") == 0) {
        strncpy(ctx->location, evt->header_value, sizeof(ctx->location) - 1);
        ctx->location[sizeof(ctx->location) - 1] = '\0';
    }
    return ESP_OK;
}

/* Location 为相对路径("/...")时拼上当前URL的 scheme://host */
static void resolve_redirect_url(const char *base, const char *location,
                                 char *out, size_t out_size)
{
    if (location[0] != '/') {
        strncpy(out, location, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    const char *host_end = strstr(base, "://");
    host_end = host_end ? strchr(host_end + 3, '/') : NULL;
    size_t origin = host_end ? (size_t)(host_end - base) : strlen(base);
    if (origin >= out_size) origin = out_size - 1;
    memcpy(out, base, origin);
    strncpy(out + origin, location, out_size - 1 - origin);
    out[out_size - 1] = '\0';
}

#define MAX_REDIRECTS 3

esp_err_t cloud_transport_get_stream(const char *url, const char *header_name,
                                     const char *header_value,
                                     cloud_transport_chunk_cb_t on_chunk, void *user_ctx,
                                     const volatile bool *cancel_flag, int *out_status)
{
    if (!url || !on_chunk) return ESP_ERR_INVALID_ARG;
    if (out_status) *out_status = 0;
    /* 不走 cloud_transport_client_create(固定POST)：GET 单独建句柄 */
    redirect_ctx_t redir = { .location = {0} };
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_GET, .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = STREAM_TIMEOUT_MS,
        .event_handler = redirect_event_handler, .user_data = &redir,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    int64_t start_us = esp_timer_get_time();
    esp_err_t err = ESP_OK;
    if (header_name && header_value) {
        err = esp_http_client_set_header(client, header_name, header_value);
    }
    char current[768];
    strncpy(current, url, sizeof(current) - 1);
    current[sizeof(current) - 1] = '\0';
    for (int hop = 0; err == ESP_OK; hop++) {
        redir.location[0] = '\0';
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "GET打开失败: %s", esp_err_to_name(err));
            break;
        }
        err = stream_prepare_response(client, cancel_flag, start_us, out_status);
        if (err == ESP_ERR_HTTP_CONNECT && *out_status >= 300 && *out_status < 400
            && redir.location[0] && hop < MAX_REDIRECTS) {
            /* 3xx：drain 后跟随 Location 重试同一句柄 */
            (void)stream_read_loop(client, discard_chunk, NULL, cancel_flag, start_us);
            esp_http_client_close(client);
            resolve_redirect_url(current, redir.location, current, sizeof(current));
            err = esp_http_client_set_url(client, current);
            continue;
        }
        if (err == ESP_ERR_HTTP_CONNECT) {
            /* 非2xx也要读到结尾，理由同 exec_request 的 drain */
            esp_err_t drain_err = stream_read_loop(client, discard_chunk, NULL,
                                                   cancel_flag, start_us);
            err = drain_err == ESP_OK ? err : drain_err;
        } else if (err == ESP_OK) {
            err = stream_read_loop(client, on_chunk, user_ctx, cancel_flag, start_us);
        }
        break;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}
