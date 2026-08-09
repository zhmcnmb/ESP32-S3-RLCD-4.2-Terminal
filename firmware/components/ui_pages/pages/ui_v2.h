#pragma once

#include "sys_state.h"

/*
 * 页面共享绘图原语：只保留被两个以上页面复用的小工具。
 * 页头有稳定的设备状态锚点；内容区以圆角卡片分组。
 */

/* 页头：可选左侧标题与右侧 SD/WiFi/电池图标。没有水平分隔线，留白由
 * 页面主内容决定；数据不可用时图标改变内部形态，不改变占位宽度。 */
void ui_v2_draw_header(const char *title, const sys_state_snapshot_t *snap);

/* 统一空状态：居中主文案(scale2) + 可选居中副文案(scale1，y+40处)。
 * secondary 可传 NULL；正常成功状态不要调用本函数 */
void ui_v2_draw_empty_state(int y, const char *primary, const char *secondary);

/* 卡片：圆角边框(radius 12)，把相关内容聚成一块。只画边框，内容由页面自绘。 */
void ui_v2_draw_card(int x, int y, int w, int h);

/* 摄氏单位：8×8圆环加放大C，返回绘制后的x坐标。 */
int ui_v2_draw_temp_unit(int x, int y);

/* 右上角反白状态徽章；text为NULL时不绘制。 */
void ui_v2_draw_status_badge(const char *text);
