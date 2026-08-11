#pragma once

/*
 * cloud_transport 组件内部头：仅供组件内部 .c 文件共享（不进 include/，不对外暴露）。
 * 声明"单次流式请求执行"的核心帮助函数——一次性路径(post_stream，见
 * cloud_transport_stream.c)和持久连接路径(cloud_transport_conn.c)共用同一份实现，
 * 区别只在于谁创建/关闭 esp_http_client 句柄。
 */

#include "esp_http_client.h"

#include "cloud_transport.h"

#define STREAM_TIMEOUT_MS 15000
#define STREAM_TOTAL_TIMEOUT_MS 45000
#define STREAM_READ_CHUNK 1024
#define STREAM_WRITE_CHUNK 4096

/* 创建一个新的 esp_http_client 句柄(POST + crt_bundle 证书校验 + STREAM_TIMEOUT_MS)，
 * 不设置header、不打开连接。失败返回 ESP_ERR_NO_MEM。 */
esp_err_t cloud_transport_client_create(const char *url, esp_http_client_handle_t *out_client);

/* 在已创建的 client 上执行一次完整的流式POST：设置url/header、写body、抓响应header、
 * 跑读循环(含总超时轮询与取消检查)。不负责创建/关闭/清理client——
 * 一次性路径每次调用后立即close+cleanup；持久连接路径复用同一句柄跨多次调用，
 * 只在显式 conn_close 时才清理。*out_status 语义与 cloud_transport_post_stream 一致。 */
esp_err_t cloud_transport_exec_request(esp_http_client_handle_t client, const char *url,
                                       const char *auth, const char *content_type,
                                       const char *accept, const void *body, size_t body_len,
                                       cloud_transport_chunk_cb_t on_chunk, void *user_ctx,
                                       const volatile bool *cancel_flag, int *out_status);
