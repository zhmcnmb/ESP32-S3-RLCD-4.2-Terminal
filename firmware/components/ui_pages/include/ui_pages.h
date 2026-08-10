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


/* 环境趋势页: 温湿度历史折线图，数据来自 sys_state */
const ui_page_t *page_env_monitor_get(void);


/* 天气页: 和风天气当前实况，数据来自 sys_state */
const ui_page_t *page_weather_get(void);

/* WiFi设置页: 连接状态显示与扫描配网入口，配网流程由 wifi_setup 组件驱动 */
const ui_page_t *page_wifi_get(void);

#ifdef __cplusplus
}
#endif
