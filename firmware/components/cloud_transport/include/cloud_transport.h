#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 云端HTTPS请求的公共底座：TLS证书校验(ESP官方cert bundle) + 可选gzip自动解压。
 * 首版只做同步阻塞GET，够用weather_client直连QWeather；P5.0接入SiliconFlow/DeepSeek
 * 需要SSE流式和取消时再扩展Interface，不预先实现未验证过的用法。
 *
 * 不持有状态、不需要init，纯函数式调用，调用方各自管理自己的轮询任务和缓存。
 */

/*
 * 阻塞发起一次HTTPS GET，证书校验走ESP官方bundle(不接受自签名/无效证书)。
 * header_name/header_value 可选自定义请求头(如API Key)，不需要则传NULL。
 * 响应若带 Content-Encoding: gzip 会自动用ROM miniz解压；解压/接收后的正文写入
 * out_buf(以'\0'结尾)，*out_len 是不含结尾'\0'的字节数。
 *
 * 失败返回:
 *   ESP_ERR_INVALID_ARG    参数为空或out_buf_size太小(至少留2字节)
 *   ESP_ERR_NO_MEM         内部接收缓冲区分配失败
 *   ESP_ERR_HTTP_CONNECT   HTTP状态码不是2xx
 *   ESP_ERR_INVALID_RESPONSE  gzip容器头解析失败或解压失败
 *   其余esp_http_client_perform()透传的网络/TLS错误(超时、证书校验失败等)
 */
esp_err_t cloud_transport_get(const char *url, const char *header_name, const char *header_value,
                               char *out_buf, size_t out_buf_size, size_t *out_len);

#ifdef __cplusplus
}
#endif
