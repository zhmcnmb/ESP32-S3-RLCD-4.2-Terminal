/*
 * cloud_transport_conn.c - 持久 TLS 连接的流式 POST。
 *
 * 同一 esp_http_client 句柄跨多次 POST 复用底层TCP/TLS连接(esp_http_client_open()在
 * 句柄未被close的情况下会跳过重新握手，见 esp_http_client_connect() 的状态检查)——
 * 消除句间(如TTS一次回答的多个合成块)重复TLS握手开销。服务端若不保持连接(主动断开
 * keep-alive)，调用方靠自己的"网络层失败→关闭重连重试一次"逻辑兜底，本文件不感知
 * 该策略、只管连接生命周期。
 * 定位：有意薄 adapter--in_use 并发守卫 + 失败 close 是真实逻辑，
 * deletion test 后复杂度回到调用方(各自管句柄生命周期)，非 pass-through。
 */
#include "cloud_transport.h"
#include "cloud_transport_internal.h"

#include <stdlib.h>

#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "cloud_transport_conn";

struct cloud_stream_conn {
    esp_http_client_handle_t client;
    bool in_use; /* 防并发误用；不加锁，调用方本就串行使用同一条连接 */
};

esp_err_t cloud_transport_conn_open(cloud_stream_conn_t **out_conn, const char *url)
{
    if (!out_conn || !url) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_conn = NULL;
    cloud_stream_conn_t *conn = calloc(1, sizeof(*conn));
    if (!conn) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = cloud_transport_client_create(url, &conn->client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "创建持久连接失败: %s", esp_err_to_name(err));
        free(conn);
        return err;
    }
    *out_conn = conn;
    return ESP_OK;
}

esp_err_t cloud_transport_conn_post_stream(cloud_stream_conn_t *conn,
        const char *url, const char *auth_header_value, const char *content_type,
        const void *body, size_t body_len,
        cloud_transport_chunk_cb_t on_chunk, void *user_ctx,
        const volatile bool *cancel_flag, int *out_status)
{
    if (out_status) *out_status = 0;
    if (!conn || !url || !on_chunk || (body_len > 0 && !body)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (conn->in_use) {
        ESP_LOGW(TAG, "并发复用被拒绝(仍有请求进行中)");
        return ESP_ERR_INVALID_STATE;
    }
    conn->in_use = true;
    esp_err_t err = cloud_transport_exec_request(conn->client, url, auth_header_value, content_type,
                                                 NULL, body, body_len, on_chunk, user_ctx,
                                                 cancel_flag, out_status);
    conn->in_use = false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "请求失败(%s)，关闭连接让下次open从干净状态开始",
                 esp_err_to_name(err));
        esp_err_t cerr = esp_http_client_close(conn->client);
        if (cerr != ESP_OK) {
            ESP_LOGW(TAG, "失败后关闭连接异常: %s", esp_err_to_name(cerr));
        }
    }
    return err;
}

void cloud_transport_conn_close(cloud_stream_conn_t *conn)
{
    if (!conn) {
        return;
    }
    if (conn->client) {
        esp_http_client_close(conn->client);
        esp_http_client_cleanup(conn->client);
    }
    free(conn);
}
