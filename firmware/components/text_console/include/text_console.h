#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 串口文本控制台：USB-Serial-JTAG(Type-C直连的原生USB，烧录/日志同一口) RX
 * 读一行文本(回车结尾)投递 sys_state 文本邮箱，voice_assistant 空闲时取走并按
 * 完整 agent/TTS 链路处理——语音链路的验收测试入口，跳过录音/ASR。
 * 日志仍走同一口 TX，收发互不冲突。
 *
 * 任务: text_console, 栈3072, 优先级2 (登记于 docs/coding-standards.md 第7节)
 */

/* 依赖 sys_state_init() 已完成；只装 UART 驱动和读取任务 */
esp_err_t text_console_init(void);

#ifdef __cplusplus
}
#endif
