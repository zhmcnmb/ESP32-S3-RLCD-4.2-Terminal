#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 串口文本控制台：USB-Serial-JTAG(Type-C直连的原生USB，烧录/日志同一口) RX
 * 读一行文本(回车结尾)：以 `>` 开头的行去掉前缀后
 * 投递 sys_state 文本邮箱，由 voice_assistant 按完整 agent/TTS 链路处理。
 * 无前缀或含控制字符的行丢弃——烧录口与注入口共用，开端口残留不能当用户问题。
 * 日志仍走同一口 TX，收发互不冲突。
 *
 * 任务: text_console, 栈3072, 优先级2 (登记于 docs/coding-standards.md 第7节)
 */

/* 依赖 sys_state_init() 已完成；只装 UART 驱动和读取任务 */
esp_err_t text_console_init(void);

#ifdef __cplusplus
}
#endif
