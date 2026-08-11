#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 云端HTTPS请求的公共底座：TLS证书校验(ESP官方cert bundle)。
 * - cloud_transport_get(): 同步阻塞GET + 可选gzip自动解压，够用weather_client直连QWeather。
 * - cloud_transport_post_stream(): 流式POST，请求体一次性传入(body/body_len)，响应体
 *   逐块回调、不整体缓冲——SiliconFlow/DeepSeek的SSE聊天流、TTS原始PCM流、ASR的
 *   multipart/form-data文件上传都靠它；不处理响应gzip(这几类响应实测不会被压缩，
 *   真出现再加，不预先实现未验证过的用法)。multipart帧(boundary/Content-Disposition)
 *   由调用方(asr_client)自己拼body，cloud_transport不理解协议细节，只管传输字节。
 *
 * 一次性 GET/POST 不持有状态、不需要init；需要跨连续POST复用TLS时，调用方显式持有
 * 下方 cloud_stream_conn_t，不影响其他云客户端的纯函数式调用方式。
 */

/*
 * 阻塞发起一次HTTPS GET，证书校验走ESP官方bundle(不接受自签名/无效证书)。
 * header_name/header_value 可选自定义请求头(如API Key)，不需要则传NULL。响应若带
 * Content-Encoding: gzip 会自动用ROM miniz解压；解压/接收后的正文写入response
 * （以'\0'结尾），out_len 是不含结尾'\0'的字节数。
 *
 * 失败返回:
 *   ESP_ERR_INVALID_ARG    参数为空或response_size太小(至少留2字节)
 *   ESP_ERR_NO_MEM         内部接收缓冲区分配失败
 *   ESP_ERR_HTTP_CONNECT   HTTP状态码不是2xx
 *   ESP_ERR_INVALID_RESPONSE  gzip容器头解析失败或解压失败
 *   其余esp_http_client_perform()透传的网络/TLS错误(超时、证书校验失败等)
 */
typedef struct {
    const char *url;
    const char *header_name;
    const char *header_value;
    char *response;
    size_t response_size;
    size_t out_len;
} cloud_transport_get_request_t;

esp_err_t cloud_transport_get(cloud_transport_get_request_t *request);

/*
 * 响应体分块回调：流式请求每收到一段响应体原始字节就调用一次，不做任何切帧/解析——
 * SSE分行、JSON提取、PCM缓冲都是调用方的事，cloud_transport只管往外递字节。
 * 回调返回非ESP_OK会中止本次传输，cloud_transport_post_stream()原样透传该错误码。
 */
typedef esp_err_t (*cloud_transport_chunk_cb_t)(const char *data, size_t len, void *ctx);

/*
 * 阻塞式流式HTTPS POST：请求体一次性传入(body/body_len)，响应体逐块交给on_chunk，
 * 不整体缓冲——SSE聊天流、TTS PCM流都可能持续几十KB到几百KB，不适合像
 * cloud_transport_get()那样先囤进一块定长buffer。不处理响应gzip(见文件头注释)。
 *
 * auth_header_value非NULL时发送 "Authorization: Bearer <value>"(SiliconFlow/DeepSeek
 * 认证方式固定如此，和weather_client那种自定义header名不同，不做成通用header_name参数)。
 * content_type是请求体MIME(如"application/json")，可选。
 *
 * cancel_flag可选：非NULL时每个请求体分片和每轮响应读取前都会检查；单次底层I/O不可
 * 抢占，因此取消延迟至多为一次HTTP I/O超时。命中后返回ESP_ERR_NOT_FINISHED。
 *
 * 成功取得HTTP响应头后*out_status会写入实际状态码(包括401/429/503/504)。连接/TLS
 * 失败时保持0；若取消或总超时发生在响应头之后，该值可能已写入，调用方须以返回错误码为准。
 *
 * 失败返回:
 *   ESP_ERR_INVALID_ARG       参数为空
 *   ESP_ERR_INVALID_SIZE      请求体长度超过esp_http_client可表达范围
 *   ESP_ERR_NO_MEM             内部资源分配失败
 *   ESP_ERR_NOT_FINISHED       cancel_flag被置位，请求被主动中止
 *   ESP_ERR_HTTP_CONNECT       HTTP状态码不是2xx(*out_status带具体码)
 *   ESP_ERR_INVALID_RESPONSE   on_chunk回调中止了传输
 *   其余esp_http_client透传的网络/TLS错误(超时、证书校验失败等)
 */
esp_err_t cloud_transport_post_stream(const char *url, const char *auth_header_value,
                                       const char *content_type, const void *body, size_t body_len,
                                       cloud_transport_chunk_cb_t on_chunk, void *user_ctx,
                                       const volatile bool *cancel_flag, int *out_status);

/*
 * 阻塞式流式HTTPS GET：响应体逐块交给on_chunk，不整体缓冲——搜索结果页等大HTML
 * (几十~上百KB)塞不进 cloud_transport_get 的定长内部缓冲。header_name/header_value
 * 可选(如User-Agent)。3xx 重定向最多跟随3次(含相对 Location)。
 * out_status/cancel_flag/错误码语义与 cloud_transport_post_stream 一致。
 */
esp_err_t cloud_transport_get_stream(const char *url, const char *header_name,
                                     const char *header_value,
                                     cloud_transport_chunk_cb_t on_chunk, void *user_ctx,
                                     const volatile bool *cancel_flag, int *out_status);

/*
 * 持久连接的流式POST：同 host 连续多次 POST(如TTS一次回答的多个合成块)复用同一条
 * TLS连接，省去每块单独握手的耗时；服务端若中途主动断开连接，由调用方重试兜底
 * (关闭旧连接、重新open、原地重试一次)，本Interface不自动重连。
 *
 * conn_open创建一条连接(不发送请求)；conn_post_stream在其上执行一次完整的流式POST，
 * 成功或失败后连接保持打开供下次复用(不主动close)；conn_close才真正关闭底层连接并
 * 释放资源，NULL容忍。conn_post_stream 不可重入——同一conn上一次调用未返回前再次
 * 调用会返回 ESP_ERR_INVALID_STATE(调用方本身应串行使用同一条连接)。
 *
 * out_status/cancel_flag/错误码语义与 cloud_transport_post_stream() 一致。
 */
typedef struct cloud_stream_conn cloud_stream_conn_t; /* 不透明持久连接 */

esp_err_t cloud_transport_conn_open(cloud_stream_conn_t **out_conn, const char *url);

esp_err_t cloud_transport_conn_post_stream(cloud_stream_conn_t *conn,
        const char *url, const char *auth_header_value, const char *content_type,
        const void *body, size_t body_len,
        cloud_transport_chunk_cb_t on_chunk, void *user_ctx,
        const volatile bool *cancel_flag, int *out_status);

void cloud_transport_conn_close(cloud_stream_conn_t *conn); /* NULL 容忍 */

#ifdef __cplusplus
}
#endif
