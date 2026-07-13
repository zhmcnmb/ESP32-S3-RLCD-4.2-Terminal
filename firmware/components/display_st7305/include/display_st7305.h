#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ST7305 全反射屏驱动，横屏400×300，1bpp。
 * 依赖 board_rlcd42 已完成SPI总线初始化(board_init)。
 * 初始化序列与像素打包公式摘自官方demo，见 docs/vendor-provenance.md。
 */

/* 创建panel IO并执行面板初始化序列，分配帧缓冲(白底) */
esp_err_t st7305_init(void);

/* 帧缓冲操作，坐标系: 左上角(0,0)，x向右0~399，y向下0~299 */
void st7305_clear(bool black);
void st7305_set_pixel(int x, int y, bool black);
void st7305_fill_rect(int x, int y, int w, int h, bool black);

/* 空心矩形，thickness为边框粗细 */
void st7305_draw_rect(int x, int y, int w, int h, int thickness, bool black);

/* 圆角空心矩形(卡片边框)。radius为圆角半径 */
void st7305_draw_round_rect(int x, int y, int w, int h, int radius, bool black);

/* 任意直线(Bresenham) */
void st7305_draw_line(int x0, int y0, int x1, int y1, bool black);

/* ASCII文本，8×8点阵，scale为整数放大倍率(1=8px, 3=24px)。
 * 覆盖 ASCII 0x20~0x5A(空格~Z)，小写自动转大写；超范围字符画空心框。 */
void st7305_draw_text(int x, int y, int scale, const char *str, bool black);

/* 中英文混排(UTF-8)。中文用16×16字库(scale=1时16px高)，ASCII用8×8放大到同高。
 * 字库只含 tools/charset.txt 里的字，缺字画空心框——新增文案需重跑 tools/gen_font.py。
 * 返回绘制后的x坐标(可用于连续拼接)。 */
int st7305_draw_utf8(int x, int y, int scale, const char *utf8, bool black);

/* 计算UTF-8字符串的像素宽度，用于居中排版 */
int st7305_utf8_width(int scale, const char *utf8);

/* 把帧缓冲整屏刷到面板 */
esp_err_t st7305_flush(void);

#ifdef __cplusplus
}
#endif
