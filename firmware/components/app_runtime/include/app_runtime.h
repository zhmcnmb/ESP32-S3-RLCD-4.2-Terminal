#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 启动编排：硬件初始化 -> 可选服务 -> UI注册 -> 异步联网。
 * 接管原 main/app_main.c 里除"打印芯片信息"外的全部启动逻辑，见
 * docs/coding-standards.md 第1节分层图；app_main.c 之后只调用这一个函数。
 *
 * 板级硬件初始化失败(board/显示/RTC/温湿度/电池/输入/UI)返回错误码，由
 * app_main 唯一决定是否 abort；可选服务(SD/校时/环境记录/联网/云客户端)
 * 失败只降级+记日志，不阻断启动。
 *
 * 密钥/凭据由调用方从 main/secrets.h 读出后传入，app_runtime 本身不依赖
 * secrets.h(避免 main <-> app_runtime 互相 REQUIRES 造成组件循环依赖)。
 */
typedef struct {
    const char *wifi_ssid;
    const char *wifi_password;
    const char *timezone;         /* POSIX格式，如 "CST-8" */
    const char *qweather_api_key;  /* 天气(P3)设备直连QWeather */
    const char *qweather_api_host; /* 不含协议前缀，如 "xxxxx.re.qweatherapi.com" */
    const char *qweather_location; /* LocationID，如 "101010100" */
    const char *siliconflow_api_key; /* P5.0 ASR/TTS/LLM，SiliconFlow平台Key */
    const char *siliconflow_llm_model; /* SiliconFlow LLM精确模型标识，不含provider URL */
    const char *siliconflow_llm_url; /* SiliconFlow LLM端点，URL须配置化 */
    const char *deepseek_api_key;    /* P5.0 LLM可选切换，留空则只用SiliconFlow */
    const char *deepseek_llm_url;    /* DeepSeek LLM端点，use_deepseek_llm 时生效 */
    bool use_deepseek_llm;            /* true=LLM走DeepSeek，false(默认)=走SiliconFlow */
} app_runtime_config_t;

esp_err_t app_runtime_start(const app_runtime_config_t *cfg);

#ifdef __cplusplus
}
#endif
