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
 * 不持有状态、不需要init，纯函数式调用，调用方各自管理自己的轮询任务和缓存。
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
 * 有界 JSON POST：固定发送 application/json 并要求 JSON 响应，响应由内部流式
 * 接收进调用方提供的 RAM 缓冲，绝不为云端响应无上限分配内存。
 * response_size 至少为2；超出缓冲预算返回 ESP_ERR_INVALID_SIZE，out_status 语义与
 * post_stream 一致。适用于搜索、正文提取等小型结构化 API，不替代 SSE/PCM 流。
 */
typedef struct {
    const char *url;
    const char *auth_header_value; /* 非NULL时发送 Authorization: Bearer <value> */
    const void *body;
    size_t body_len;
    char *response;
    size_t response_size;
    const volatile bool *cancel_flag;
} cloud_transport_json_request_t;

esp_err_t cloud_transport_post_json(const cloud_transport_json_request_t *request,
                                    size_t *out_len, int *out_status);

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
 * cancel_flag可选：非NULL时读循环每隔(至多)HTTP读超时间隔检查一次，一旦被置true就
 * 立即关闭连接、返回 ESP_ERR_NOT_FINISHED(语义借用为"被主动中止"，仓库内一处调用)。
 *
 * *out_status在HTTP握手成功后总会写入实际状态码(包括401/429/503/504这类非2xx，供
 * 调用方分类重试)；网络层失败(连接/TLS/超时/取消)时*out_status保持0，调用方不应读取。
 *
 * 失败返回:
 *   ESP_ERR_INVALID_ARG       参数为空
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

#ifdef __cplusplus
}
#endif
