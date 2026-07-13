#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PCF85063 RTC (I2C 0x51)，有独立备用电池，断主电仍走时。
 * 依赖 board_init() 已完成。
 *
 * 注意: 符号必须用 rtc_pcf85063_ 全前缀——ESP-IDF 内部已有 rtc_init 符号，
 * 用 rtc_ 短前缀会链接冲突。 */

esp_err_t rtc_pcf85063_init(void);

/* 读取RTC时间(UTC)。芯片内部用BCD，此处已转为标准struct tm */
esp_err_t rtc_pcf85063_get_time(struct tm *out);

/* 写入RTC时间(UTC) */
esp_err_t rtc_pcf85063_set_time(const struct tm *t);

/* RTC是否掉电过(振荡器停止标志)。false表示时间不可信，需要NTP重新校时 */
esp_err_t rtc_pcf85063_is_valid(bool *valid);

#ifdef __cplusplus
}
#endif
