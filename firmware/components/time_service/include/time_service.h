#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 系统时间协调。策略:
 *   启动时  -> 从RTC读时间设进系统时钟(无网也能立即显示时间)
 *   联网后  -> SNTP校时，成功则写回RTC(断电也不丢)
 *
 * 页面直接用标准 time()/localtime() 即可，不需要调用本组件。
 */

/* 从RTC恢复系统时间，并设置时区(POSIX格式，如"CST-8") */
esp_err_t time_service_init(const char *timezone);

/* 启动SNTP异步校时。需已联网。成功后自动写回RTC */
esp_err_t time_service_start_sntp(void);

/* 时间是否可信(RTC有效 或 已NTP同步过) */
bool time_service_is_valid(void);

#ifdef __cplusplus
}
#endif
