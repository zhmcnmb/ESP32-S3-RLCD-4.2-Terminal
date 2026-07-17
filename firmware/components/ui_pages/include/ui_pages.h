#pragma once

#include "ui_page.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 各功能页面的获取入口。由 app_runtime 注册进 ui_manager。
 * 新增页面: 在 pages/ 下新建 page_xxx.c，实现 ui_page_t，在此暴露 getter。
 */

/* 默认页: Jarvis 头像与语音/agent 状态 */
const ui_page_t *page_jarvis_get(void);

/* 时钟页: 时间、日期星期、室内温湿度 */
const ui_page_t *page_home_get(void);

/* 诊断页: 旧式直读硬件自检，不得作为新页面模板，也不注册进翻页 */
const ui_page_t *page_diag_get(void);

/* 环境趋势页: 温湿度历史折线图，数据来自 sys_state */
const ui_page_t *page_env_monitor_get(void);

/* Claude用量页: 5小时/7天订阅用量窗口；Mac 离线独立降级 */
const ui_page_t *page_quota_get(void);

/* 天气页: 和风天气当前实况，数据来自 sys_state */
const ui_page_t *page_weather_get(void);

#ifdef __cplusplus
}
#endif
