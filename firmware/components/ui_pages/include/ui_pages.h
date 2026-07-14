#pragma once

#include "ui_page.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 各功能页面的获取入口。app_main 取到后注册进 ui_manager。
 * 新增页面: 在 pages/ 下新建 page_xxx.c，实现 ui_page_t，在此暴露 getter。
 */

/* 首页: 时钟、日期星期、室内温湿度、网络状态 */
const ui_page_t *page_home_get(void);

/* 诊断页: 硬件自检结果。也是页面接口的参照范例 */
const ui_page_t *page_diag_get(void);

/* 环境趋势页: 温湿度历史折线图，数据来自 env_logger */
const ui_page_t *page_env_monitor_get(void);

/* Claude用量页: 5小时/7天订阅用量窗口，数据来自 claude_quota_client */
const ui_page_t *page_quota_get(void);

/* 天气页: 和风天气当前实况，数据来自 weather_client */
const ui_page_t *page_weather_get(void);

/* 后续阶段补充:
 * const ui_page_t *page_voice_get(void);         P5 语音交互
 */

#ifdef __cplusplus
}
#endif
