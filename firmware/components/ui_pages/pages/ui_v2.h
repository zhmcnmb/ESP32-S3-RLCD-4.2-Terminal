#pragma once

#include "sys_state.h"

/*
 * 页面共享绘图原语：只保留被两个以上页面复用的小工具。
 * 页头有稳定的设备状态锚点；内容布局不强行套进统一取景框。
 */

/* 页头：可选左侧标题与右侧 SD/WiFi/电池图标。没有水平分隔线，留白由
 * 页面主内容决定；数据不可用时图标改变内部形态，不改变占位宽度。 */
void ui_v2_draw_header(const char *title, const sys_state_snapshot_t *snap);

/* 统一空状态：居中主文案(scale2) + 可选居中副文案(scale1，y+40处)。
 * secondary 可传 NULL；正常成功状态不要调用本函数 */
void ui_v2_draw_empty_state(int y, const char *primary, const char *secondary);
