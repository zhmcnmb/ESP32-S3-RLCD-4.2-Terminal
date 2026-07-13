#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 16x16 中文点阵字库。数据由 tools/gen_font.py 从系统字体生成。
 * 每字 32 字节: 16行 × 每行2字节，bit7为最左像素。 */

typedef struct {
    uint16_t code;      /* Unicode 码点 */
    uint8_t bitmap[32];
} font_cn16_glyph_t;

extern const font_cn16_glyph_t FONT_CN16[];
extern const int FONT_CN16_COUNT;

#ifdef __cplusplus
}
#endif
