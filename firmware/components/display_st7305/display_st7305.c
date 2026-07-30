#include "display_st7305.h"

#include <stdlib.h>
#include <string.h>

#include "board_rlcd42.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
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
    if (!s_fb) {
        esp_lcd_panel_io_del(s_io);
        s_io = NULL;
        ESP_LOGE(TAG, "fb alloc");
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb, 0xFF, FB_SIZE);

    panel_reset();
    s_tx_err = ESP_OK;
    panel_init_sequence();
    if (s_tx_err != ESP_OK) {
        heap_caps_free(s_fb);
        s_fb = NULL;
        esp_lcd_panel_io_del(s_io);
        s_io = NULL;
        ESP_LOGE(TAG, "init sequence failed");
        return s_tx_err;
    }

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


esp_err_t st7305_flush(void)
{
    s_tx_err = ESP_OK;
    send_cmd(0x2A); send_data(0x12); send_data(0x2A);   /* 列窗口 */
    send_cmd(0x2B); send_data(0x00); send_data(0xC7);   /* 行窗口 */
    send_cmd(0x2C);                                     /* 写显存 */
    ESP_RETURN_ON_ERROR(s_tx_err, TAG, "flush cmd failed");

    return esp_lcd_panel_io_tx_color(s_io, -1, s_fb, FB_SIZE);
}
