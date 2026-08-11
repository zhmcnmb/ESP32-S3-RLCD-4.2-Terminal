#include "board_rlcd42.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static bool s_inited = false;

static esp_err_t init_i2c(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA_PIN,
        .scl_io_num = BOARD_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c_bus);
}

static esp_err_t init_lcd_spi_bus(void)
{
    spi_bus_config_t cfg = {
        .mosi_io_num = BOARD_LCD_MOSI_PIN,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_LCD_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        /* 全屏帧缓冲一次性DMA发送: 400*300/8 = 15000字节 */
        .max_transfer_sz = BOARD_LCD_WIDTH * BOARD_LCD_HEIGHT / 8,
    };
    return spi_bus_initialize(BOARD_LCD_SPI_HOST, &cfg, SPI_DMA_CH_AUTO);
}

static esp_err_t init_buttons(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_BTN_BOOT_PIN) | (1ULL << BOARD_BTN_KEY_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

esp_err_t board_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "i2c init failed");
    if (init_lcd_spi_bus() != ESP_OK) {
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        ESP_LOGE(TAG, "lcd spi bus init failed");
        return ESP_FAIL;
    }
    if (init_buttons() != ESP_OK) {
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        spi_bus_free(BOARD_LCD_SPI_HOST);
        ESP_LOGE(TAG, "buttons init failed");
        return ESP_FAIL;
    }
    s_inited = true;
    ESP_LOGI(TAG, "board init OK (I2C sda=%d scl=%d, LCD spi host=%d, btn boot=%d key=%d)",
             BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN, BOARD_LCD_SPI_HOST,
             BOARD_BTN_BOOT_PIN, BOARD_BTN_KEY_PIN);
    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_i2c_bus;
}

bool board_btn_boot_pressed(void)
{
    return gpio_get_level(BOARD_BTN_BOOT_PIN) == 0;
}

bool board_btn_key_pressed(void)
{
    return gpio_get_level(BOARD_BTN_KEY_PIN) == 0;
}
