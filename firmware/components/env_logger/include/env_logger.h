#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 温湿度历史：后台采样 SHTC3，内存环形缓冲经 sys_state 给趋势页；
 * SD 已挂载时追加 CSV，未挂载时仅内存降级。 */

/* 启动后台采样任务。CSV文件: /sdcard/env.csv，未挂载SD卡时跳过落盘 */
esp_err_t env_logger_init(void);

#ifdef __cplusplus
}
#endif
