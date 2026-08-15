#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * WiFi 配网：扫描附近AP、按键选择、逐位输入密码、切换连接并持久化到NVS。
 *
 * 状态机 OFF -> SCANNING -> AP_LIST -> PASSWORD -> CONNECTING，成功后回OFF，
 * 失败进 FAILED 可重输密码。页面(page_wifi)只读 sys_state 快照、投递命令；
 * 命令由本组件任务消费并按当前状态解释，页面不感知状态细节。
 *
 * 候选字符序列向页面公开：索引 [0, WIFI_SETUP_CHARSET_LEN) 是字符本身，
 * WIFI_SETUP_SEL_DEL / WIFI_SETUP_SEL_OK 是两个虚拟项(删除/完成)，
 * 页面按索引渲染，确认后候选停在原地方便连续输入相邻字符。
 *
 * 任务: wifi_setup_task, 栈4096, 优先级3
 */

/* 10数字 + 26大写 + 26小写 + 16符号 = 78字符；全部落在ASCII字库0x20~0x7A内 */
#define WIFI_SETUP_CHARSET "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.,_-@!#$%&*+?="
#define WIFI_SETUP_CHARSET_LEN ((int)sizeof(WIFI_SETUP_CHARSET) - 1)
#define WIFI_SETUP_SEL_DEL   (WIFI_SETUP_CHARSET_LEN)
#define WIFI_SETUP_SEL_OK    (WIFI_SETUP_CHARSET_LEN + 1)
#define WIFI_SETUP_SEL_COUNT (WIFI_SETUP_CHARSET_LEN + 2)

#define WIFI_SETUP_SSID_MAX 33 /* 32字节SSID + '\0' */
#define WIFI_SETUP_PWD_MAX  64 /* WPA2最长63 + '\0' */

/* 依赖 net_init() 已成功(WiFi驱动就绪)；只建任务与默认视图，不触碰WiFi */
esp_err_t wifi_setup_init(void);

#ifdef __cplusplus
}
#endif
