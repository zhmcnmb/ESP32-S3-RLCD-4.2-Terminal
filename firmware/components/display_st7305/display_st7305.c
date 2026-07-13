#include "display_st7305.h"

#include <stdlib.h>
#include <string.h>

#include "board_rlcd42.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "font_cn16.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "st7305";

/* 400*300 / 8 = 15000字节，1bpp。bit=1为白，bit=0为黑(与官方ColorWhite=0xff一致) */
#define FB_SIZE (BOARD_LCD_WIDTH * BOARD_LCD_HEIGHT / 8)

static esp_lcd_panel_io_handle_t s_io = NULL;
static uint8_t *s_fb = NULL;

/* 初始化序列是几十条连续写入，逐条判错会淹没代码；
 * 这里累积错误状态，由 st7305_init() 统一返回给调用方（驱动层不得abort整机） */
static esp_err_t s_tx_err = ESP_OK;

static void send_cmd(uint8_t cmd)
{
    esp_err_t err = esp_lcd_panel_io_tx_param(s_io, cmd, NULL, 0);
    if (err != ESP_OK && s_tx_err == ESP_OK) {
        s_tx_err = err;
    }
}

static void send_data(uint8_t data)
{
    esp_err_t err = esp_lcd_panel_io_tx_param(s_io, -1, &data, 1);
    if (err != ESP_OK && s_tx_err == ESP_OK) {
        s_tx_err = err;
    }
}

static void panel_reset(void)
{
    gpio_set_level(BOARD_LCD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(BOARD_LCD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_LCD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* 初始化序列摘自官方 display_bsp.cpp (commit b2ca44c)，寄存器值不可改动 */
static void panel_init_sequence(void)
{
    send_cmd(0xD6); send_data(0x17); send_data(0x02);              /* NVM Load Control */
    send_cmd(0xD1); send_data(0x01);                               /* Booster Enable */
    send_cmd(0xC0); send_data(0x11); send_data(0x04);              /* Gate Voltage */
    send_cmd(0xC1); send_data(0x69); send_data(0x69); send_data(0x69); send_data(0x69); /* VSHP */
    send_cmd(0xC2); send_data(0x19); send_data(0x19); send_data(0x19); send_data(0x19);
    send_cmd(0xC4); send_data(0x4B); send_data(0x4B); send_data(0x4B); send_data(0x4B);
    send_cmd(0xC5); send_data(0x19); send_data(0x19); send_data(0x19); send_data(0x19);
    send_cmd(0xD8); send_data(0x80); send_data(0xE9);
    send_cmd(0xB2); send_data(0x02);
    send_cmd(0xB3);
    send_data(0xE5); send_data(0xF6); send_data(0x05); send_data(0x46); send_data(0x77);
    send_data(0x77); send_data(0x77); send_data(0x77); send_data(0x76); send_data(0x45);
    send_cmd(0xB4);
    send_data(0x05); send_data(0x46); send_data(0x77); send_data(0x77);
    send_data(0x77); send_data(0x77); send_data(0x76); send_data(0x45);
    send_cmd(0x62); send_data(0x32); send_data(0x03); send_data(0x1F);
    send_cmd(0xB7); send_data(0x13);
    send_cmd(0xB0); send_data(0x64);
    send_cmd(0x11);                                                /* Sleep Out */
    vTaskDelay(pdMS_TO_TICKS(200));
    send_cmd(0xC9); send_data(0x00);
    send_cmd(0x36); send_data(0x48);                               /* MADCTL */
    send_cmd(0x3A); send_data(0x11);                               /* 像素格式 */
    send_cmd(0xB9); send_data(0x20);
    send_cmd(0xB8); send_data(0x29);
    send_cmd(0x21);                                                /* 反显开 */
    send_cmd(0x2A); send_data(0x12); send_data(0x2A);              /* 列窗口 */
    send_cmd(0x2B); send_data(0x00); send_data(0xC7);              /* 行窗口 */
    send_cmd(0x35); send_data(0x00);                               /* TE开 */
    send_cmd(0xD0); send_data(0xFF);
    send_cmd(0x38);                                                /* Idle off */
    send_cmd(0x29);                                                /* Display On */
}

esp_err_t st7305_init(void)
{
    ESP_RETURN_ON_FALSE(s_io == NULL, ESP_ERR_INVALID_STATE, TAG, "already inited");

    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << BOARD_LCD_RST_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "rst gpio");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_DC_PIN,
        .cs_gpio_num = BOARD_LCD_CS_PIN,
        .pclk_hz = 10 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_cfg, &s_io),
        TAG, "panel io");

    s_fb = heap_caps_malloc(FB_SIZE, MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(s_fb, ESP_ERR_NO_MEM, TAG, "fb alloc");
    memset(s_fb, 0xFF, FB_SIZE);

    panel_reset();
    s_tx_err = ESP_OK;
    panel_init_sequence();
    ESP_RETURN_ON_ERROR(s_tx_err, TAG, "init sequence failed");

    ESP_LOGI(TAG, "init OK, fb=%d bytes", FB_SIZE);
    return ESP_OK;
}

void st7305_clear(bool black)
{
    memset(s_fb, black ? 0x00 : 0xFF, FB_SIZE);
}

/* 横屏像素打包(官方公式): 每字节管2列×4行，位序交错 */
void st7305_set_pixel(int x, int y, bool black)
{
    if (x < 0 || x >= BOARD_LCD_WIDTH || y < 0 || y >= BOARD_LCD_HEIGHT) {
        return;
    }
    int inv_y = BOARD_LCD_HEIGHT - 1 - y;
    int index = (x / 2) * (BOARD_LCD_HEIGHT / 4) + inv_y / 4;
    int bit = 7 - ((inv_y % 4) * 2 + x % 2);
    if (black) {
        s_fb[index] &= ~(1 << bit);
    } else {
        s_fb[index] |= (1 << bit);
    }
}

void st7305_fill_rect(int x, int y, int w, int h, bool black)
{
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            st7305_set_pixel(i, j, black);
        }
    }
}

void st7305_draw_rect(int x, int y, int w, int h, int thickness, bool black)
{
    st7305_fill_rect(x, y, w, thickness, black);                  /* 上 */
    st7305_fill_rect(x, y + h - thickness, w, thickness, black);  /* 下 */
    st7305_fill_rect(x, y, thickness, h, black);                  /* 左 */
    st7305_fill_rect(x + w - thickness, y, thickness, h, black);  /* 右 */
}

void st7305_draw_line(int x0, int y0, int x1, int y1, bool black)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        st7305_set_pixel(x0, y0, black);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void st7305_draw_round_rect(int x, int y, int w, int h, int r, bool black)
{
    /* 四条直边 */
    st7305_fill_rect(x + r, y, w - 2 * r, 1, black);
    st7305_fill_rect(x + r, y + h - 1, w - 2 * r, 1, black);
    st7305_fill_rect(x, y + r, 1, h - 2 * r, black);
    st7305_fill_rect(x + w - 1, y + r, 1, h - 2 * r, black);

    /* 四个圆角: 中点画圆算法的1/4象限 */
    int cx = r, cy = 0, err = 0;
    while (cx >= cy) {
        st7305_set_pixel(x + r - cx, y + r - cy, black);
        st7305_set_pixel(x + r - cy, y + r - cx, black);
        st7305_set_pixel(x + w - 1 - r + cx, y + r - cy, black);
        st7305_set_pixel(x + w - 1 - r + cy, y + r - cx, black);
        st7305_set_pixel(x + r - cx, y + h - 1 - r + cy, black);
        st7305_set_pixel(x + r - cy, y + h - 1 - r + cx, black);
        st7305_set_pixel(x + w - 1 - r + cx, y + h - 1 - r + cy, black);
        st7305_set_pixel(x + w - 1 - r + cy, y + h - 1 - r + cx, black);

        cy++;
        err += 1 + 2 * cy;
        if (2 * (err - cx) + 1 > 0) {
            cx--;
            err += 1 - 2 * cx;
        }
    }
}

/* 8×8点阵字体，覆盖ASCII 0x20(space)~0x5A(Z)，bit7为最左像素。
 * 索引 = ch - 0x20。小写字母在绘制时转为大写，超范围字符画空心框。
 * 注意: 不含中文，中文需等LVGL接入后用字库 */
#define FONT_FIRST_CH 0x20
#define FONT_LAST_CH  0x5A

static const uint8_t FONT[FONT_LAST_CH - FONT_FIRST_CH + 1][8] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* space */
    { 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00 }, /* ! */
    { 0x66, 0x66, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* " */
    { 0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00 }, /* # */
    { 0x18, 0x3E, 0x60, 0x3C, 0x06, 0x7C, 0x18, 0x00 }, /* $ */
    { 0x62, 0x66, 0x0C, 0x18, 0x30, 0x66, 0x46, 0x00 }, /* % */
    { 0x3C, 0x66, 0x3C, 0x38, 0x67, 0x66, 0x3F, 0x00 }, /* & */
    { 0x18, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* ' */
    { 0x0C, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00 }, /* ( */
    { 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00 }, /* ) */
    { 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00 }, /* * */
    { 0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00 }, /* + */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30 }, /* , */
    { 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00 }, /* - */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00 }, /* . */
    { 0x06, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x60, 0x00 }, /* / */
    { 0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00 }, /* 0 */
    { 0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00 }, /* 1 */
    { 0x3C, 0x66, 0x06, 0x0C, 0x18, 0x30, 0x7E, 0x00 }, /* 2 */
    { 0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00 }, /* 3 */
    { 0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x0C, 0x00 }, /* 4 */
    { 0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00 }, /* 5 */
    { 0x3C, 0x66, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00 }, /* 6 */
    { 0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00 }, /* 7 */
    { 0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00 }, /* 8 */
    { 0x3C, 0x66, 0x66, 0x3E, 0x06, 0x66, 0x3C, 0x00 }, /* 9 */
    { 0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00 }, /* : */
    { 0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x30, 0x00 }, /* ; */
    { 0x0C, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0C, 0x00 }, /* < */
    { 0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00 }, /* = */
    { 0x30, 0x18, 0x0C, 0x06, 0x0C, 0x18, 0x30, 0x00 }, /* > */
    { 0x3C, 0x66, 0x06, 0x0C, 0x18, 0x00, 0x18, 0x00 }, /* ? */
    { 0x3C, 0x66, 0x6E, 0x6E, 0x60, 0x62, 0x3C, 0x00 }, /* @ */
    { 0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x00 }, /* A */
    { 0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00 }, /* B */
    { 0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00 }, /* C */
    { 0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00 }, /* D */
    { 0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00 }, /* E */
    { 0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00 }, /* F */
    { 0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3E, 0x00 }, /* G */
    { 0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00 }, /* H */
    { 0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00 }, /* I */
    { 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x6C, 0x38, 0x00 }, /* J */
    { 0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x00 }, /* K */
    { 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00 }, /* L */
    { 0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00 }, /* M */
    { 0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x66, 0x00 }, /* N */
    { 0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00 }, /* O */
    { 0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00 }, /* P */
    { 0x3C, 0x66, 0x66, 0x66, 0x6E, 0x3C, 0x0E, 0x00 }, /* Q */
    { 0x7C, 0x66, 0x66, 0x7C, 0x78, 0x6C, 0x66, 0x00 }, /* R */
    { 0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00 }, /* S */
    { 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00 }, /* T */
    { 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00 }, /* U */
    { 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00 }, /* V */
    { 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00 }, /* W */
    { 0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00 }, /* X */
    { 0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00 }, /* Y */
    { 0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00 }, /* Z */
};

static const uint8_t *find_glyph(char ch)
{
    if (ch >= 'a' && ch <= 'z') {
        ch -= 32; /* 小写转大写 */
    }
    if (ch < FONT_FIRST_CH || ch > FONT_LAST_CH) {
        return NULL;
    }
    return FONT[(int)ch - FONT_FIRST_CH];
}

void st7305_draw_text(int x, int y, int scale, const char *str, bool black)
{
    if (scale < 1) {
        scale = 1;
    }
    int cx = x;
    for (const char *p = str; *p; p++) {
        const uint8_t *rows = find_glyph(*p);
        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 8; col++) {
                bool on = rows ? (rows[row] >> (7 - col)) & 1
                               /* 未收录字符: 空心框 */
                               : (row == 0 || row == 7 || col == 0 || col == 7);
                if (on) {
                    st7305_fill_rect(cx + col * scale, y + row * scale, scale, scale, black);
                }
            }
        }
        cx += 8 * scale;
    }
}

/* 解码一个UTF-8字符，返回Unicode码点，*len 输出该字符占用的字节数。
 * 非法序列按单字节跳过，避免死循环 */
static uint32_t utf8_next(const char *s, int *len)
{
    uint8_t c = (uint8_t)s[0];
    if (c < 0x80) {
        *len = 1;
        return c;
    }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *len = 2;
        return ((c & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *len = 3;
        return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    *len = 1;
    return 0xFFFD; /* 非法序列 */
}

/* 字库按码点升序排列，二分查找 */
static const uint8_t *find_cn_glyph(uint32_t code)
{
    if (code > 0xFFFF) {
        return NULL;
    }
    int lo = 0, hi = FONT_CN16_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t c = FONT_CN16[mid].code;
        if (c == code) {
            return FONT_CN16[mid].bitmap;
        }
        if (c < code) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

/* 16×16位图: 每行2字节 */
static void draw_cn_glyph(int x, int y, int scale, const uint8_t *bitmap, bool black)
{
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 16; col++) {
            uint8_t byte = bitmap[row * 2 + col / 8];
            if ((byte >> (7 - col % 8)) & 1) {
                st7305_fill_rect(x + col * scale, y + row * scale, scale, scale, black);
            }
        }
    }
}

/* ASCII的8×8放大到16px高，与中文对齐 */
static void draw_ascii_as16(int x, int y, int scale, char ch, bool black)
{
    const uint8_t *rows = find_glyph(ch);
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            bool on = rows ? (rows[row] >> (7 - col)) & 1
                           : (row == 0 || row == 7 || col == 0 || col == 7);
            if (on) {
                /* 纵向2倍拉伸到16px高，横向保持8px宽(半角) */
                st7305_fill_rect(x + col * scale, y + row * 2 * scale, scale, scale * 2, black);
            }
        }
    }
}

int st7305_draw_utf8(int x, int y, int scale, const char *utf8, bool black)
{
    if (scale < 1) {
        scale = 1;
    }
    int cx = x;
    for (const char *p = utf8; *p;) {
        int len = 0;
        uint32_t code = utf8_next(p, &len);
        p += len;

        if (code < 0x80) {
            draw_ascii_as16(cx, y, scale, (char)code, black);
            cx += 8 * scale; /* 半角 */
        } else {
            const uint8_t *glyph = find_cn_glyph(code);
            if (glyph) {
                draw_cn_glyph(cx, y, scale, glyph, black);
            } else {
                /* 缺字: 空心框，提示需要更新 charset.txt */
                st7305_fill_rect(cx, y, 16 * scale, scale, black);
                st7305_fill_rect(cx, y + 15 * scale, 16 * scale, scale, black);
                st7305_fill_rect(cx, y, scale, 16 * scale, black);
                st7305_fill_rect(cx + 15 * scale, y, scale, 16 * scale, black);
            }
            cx += 16 * scale; /* 全角 */
        }
    }
    return cx;
}

int st7305_utf8_width(int scale, const char *utf8)
{
    if (scale < 1) {
        scale = 1;
    }
    int w = 0;
    for (const char *p = utf8; *p;) {
        int len = 0;
        uint32_t code = utf8_next(p, &len);
        p += len;
        w += (code < 0x80 ? 8 : 16) * scale;
    }
    return w;
}

esp_err_t st7305_flush(void)
{
    s_tx_err = ESP_OK;
    send_cmd(0x2A); send_data(0x12); send_data(0x2A);   /* 列窗口 */
    send_cmd(0x2B); send_data(0x00); send_data(0xC7);   /* 行窗口 */
    send_cmd(0x2C);                                     /* 写显存 */
    ESP_RETURN_ON_ERROR(s_tx_err, TAG, "flush cmd failed");

    return esp_lcd_panel_io_tx_color(s_io, -1, s_fb, FB_SIZE);
}
