/* cloud_transport_stream.c - 有取消、总超时和响应预算的 HTTPS POST。 */
#include "cloud_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "cloud_transport";

#define STREAM_TIMEOUT_MS 15000
#define STREAM_TOTAL_TIMEOUT_MS 45000
#define STREAM_READ_CHUNK 1024
#define STREAM_WRITE_CHUNK 4096

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} json_response_t;

static esp_err_t stream_open_request(const char *url, const char *auth, const char *content_type,
                                     const char *accept, const void *body, size_t body_len,
                                     esp_http_client_handle_t *out_client)
{
    char auth_buf[192];
    if (auth) {
        int n = snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", auth);
        if (n < 0 || (size_t)n >= sizeof(auth_buf)) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_POST, .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = STREAM_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    if (auth) esp_http_client_set_header(client, "Authorization", auth_buf);
    if (content_type) esp_http_client_set_header(client, "Content-Type", content_type);
    if (accept) esp_http_client_set_header(client, "Accept", accept);

    esp_err_t err = esp_http_client_open(client, (int)body_len);
    if (err == ESP_OK && body && body_len > 0) {
        const char *data = body;
        for (size_t off = 0; off < body_len;) {
            size_t chunk = body_len - off;
            if (chunk > STREAM_WRITE_CHUNK) chunk = STREAM_WRITE_CHUNK;
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
        ESP_LOGW(TAG, "打开或写入失败: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }
    *out_client = client;
    return ESP_OK;
}

static esp_err_t stream_prepare_response(esp_http_client_handle_t client, int64_t start_us,
                                         int *out_status)
{
    if ((esp_timer_get_time() - start_us) / 1000 >= STREAM_TOTAL_TIMEOUT_MS) {
        return ESP_ERR_TIMEOUT;
    }
    if (esp_http_client_fetch_headers(client) < 0) {
        return ESP_FAIL;
    }
    int status = esp_http_client_get_status_code(client);
    if (out_status) *out_status = status;
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP状态非2xx: %d", status);
        return ESP_ERR_HTTP_CONNECT;
    }
    return ((esp_timer_get_time() - start_us) / 1000 < STREAM_TOTAL_TIMEOUT_MS)
           ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t stream_read_loop(esp_http_client_handle_t client, cloud_transport_chunk_cb_t on_chunk,
                                  void *user_ctx, const volatile bool *cancel_flag, int64_t start_us)
{
    char *buf = malloc(STREAM_READ_CHUNK);
    if (!buf) return ESP_ERR_NO_MEM;
    esp_err_t result = ESP_OK;
    int eagain_count = 0;
    while (true) {
        if ((esp_timer_get_time() - start_us) / 1000 >= STREAM_TOTAL_TIMEOUT_MS) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        if (cancel_flag && *cancel_flag) {
            result = ESP_ERR_NOT_FINISHED;
            break;
        }
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

static esp_err_t json_chunk(const char *data, size_t len, void *ctx)
{
    json_response_t *response = ctx;
    if (len > response->cap - 1 - response->len) return ESP_ERR_INVALID_SIZE;
    memcpy(response->buf + response->len, data, len);
    response->len += len;
    response->buf[response->len] = '\0';
    return ESP_OK;
}

static esp_err_t run_stream_request(const char *url, const char *auth, const char *content_type,
                                    const char *accept, const void *body, size_t body_len,
                                    cloud_transport_chunk_cb_t on_chunk, void *user_ctx,
                                    const volatile bool *cancel_flag, int *out_status)
{
    int64_t start_us = esp_timer_get_time();
    esp_http_client_handle_t client = NULL;
    esp_err_t err = stream_open_request(url, auth, content_type, accept, body, body_len, &client);
    if (err == ESP_OK) err = stream_prepare_response(client, start_us, out_status);
    if (err == ESP_OK) err = stream_read_loop(client, on_chunk, user_ctx, cancel_flag, start_us);
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

esp_err_t cloud_transport_post_json(const cloud_transport_json_request_t *request,
                                    size_t *out_len, int *out_status)
{
    if (!request || !request->url || !request->body || request->body_len == 0
        || !request->response || request->response_size < 2 || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_status) *out_status = 0;
    json_response_t response = {
        .buf = request->response, .cap = request->response_size,
    };
    response.buf[0] = '\0';
    esp_err_t err = run_stream_request(request->url, request->auth_header_value,
                                       "application/json", "application/json",
                                       request->body, request->body_len, json_chunk, &response,
                                       request->cancel_flag, out_status);
    *out_len = response.len;
    return err;
}
