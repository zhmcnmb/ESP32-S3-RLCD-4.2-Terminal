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
