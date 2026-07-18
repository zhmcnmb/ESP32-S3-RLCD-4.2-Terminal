#pragma once

#include <stdbool.h>

/* 状态栏小图标。单色屏没有颜色可用，靠形状区分，统一16px高。
 * 只在 ui_pages 内部使用，不对外暴露 */

/* 电池: 外框+电量填充，percent 0~100，charging时画闪电 */
void icon_battery(int x, int y, int percent, bool charging);

/* WiFi: 三段弧+圆点。level 0=断开(画叉) 1=弱 2=中 3=强 */
void icon_wifi(int x, int y, int level);

/* SD卡: 切角矩形 */
void icon_sd(int x, int y, bool present);

/* 温度计: 细杆+底部圆球，约12×18px */
void icon_thermometer(int x, int y);

/* 水滴: 尖顶+圆润底部，约12×16px */
void icon_droplet(int x, int y);

/* 风：三条横线，右端上挑小钩，约18×16px。 */
void icon_wind(int x, int y);

/* 天气现象图标，40×40，按condition关键字匹配(雨/雪/雾/云|阴/晴)，
 * 未收录类别画通用圆形标记，不引入位图资源(规范第10节) */
void icon_weather(int x, int y, const char *condition);
