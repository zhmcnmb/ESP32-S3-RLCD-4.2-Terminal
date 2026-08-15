#include "rtc_pcf85063.h"

#include "board_rlcd42.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "rtc";

/* 寄存器地址 (datasheet) */
#define REG_CTRL1    0x00
#define REG_SECONDS  0x04  /* 起始寄存器: 秒/分/时/日/星期/月/年 连续7字节 */

/* 秒寄存器的bit7是振荡器停止标志(OS)，置位表示掉电过、时间不可信 */
#define SEC_OS_FLAG  0x80

static i2c_master_dev_handle_t s_dev = NULL;

static uint8_t bcd_to_bin(uint8_t bcd)
{
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

static uint8_t bin_to_bcd(uint8_t bin)
{
    return ((bin / 10) << 4) | (bin % 10);
}

static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 100);
}

esp_err_t rtc_pcf85063_init(void)
{
    ESP_RETURN_ON_FALSE(board_i2c_bus(), ESP_ERR_INVALID_STATE, TAG, "board not inited");

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_I2C_ADDR_PCF85063,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(board_i2c_bus(), &cfg, &s_dev), TAG, "add dev");

    /* Control_1: 正常模式、24小时制、时钟运行 */
    uint8_t init[2] = { REG_CTRL1, 0x00 };
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, init, sizeof(init), 100), TAG, "ctrl1");

    ESP_LOGI(TAG, "init OK");
    return ESP_OK;
}

esp_err_t rtc_pcf85063_get_time(struct tm *out)
{
    ESP_RETURN_ON_FALSE(out && s_dev, ESP_ERR_INVALID_ARG, TAG, "bad arg");

    uint8_t buf[7] = { 0 };
    ESP_RETURN_ON_ERROR(read_regs(REG_SECONDS, buf, sizeof(buf)), TAG, "read");

    out->tm_sec = bcd_to_bin(buf[0] & 0x7F); /* 屏蔽OS标志位 */
    out->tm_min = bcd_to_bin(buf[1] & 0x7F);
    out->tm_hour = bcd_to_bin(buf[2] & 0x3F);
    out->tm_mday = bcd_to_bin(buf[3] & 0x3F);
    out->tm_wday = buf[4] & 0x07;
    out->tm_mon = bcd_to_bin(buf[5] & 0x1F) - 1;   /* 芯片1-12, tm是0-11 */
    out->tm_year = bcd_to_bin(buf[6]) + 100;       /* 芯片00-99代表2000-2099, tm以1900为基 */
    out->tm_isdst = 0;

    return ESP_OK;
}

esp_err_t rtc_pcf85063_set_time(const struct tm *t)
{
    ESP_RETURN_ON_FALSE(t && s_dev, ESP_ERR_INVALID_ARG, TAG, "bad arg");

    uint8_t buf[8] = {
        REG_SECONDS,
        bin_to_bcd(t->tm_sec),   /* 写0到bit7同时清除OS标志 */
        bin_to_bcd(t->tm_min),
        bin_to_bcd(t->tm_hour),
        bin_to_bcd(t->tm_mday),
        (uint8_t)(t->tm_wday & 0x07),
        bin_to_bcd(t->tm_mon + 1),
        bin_to_bcd((uint8_t)(t->tm_year % 100)),
    };
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, buf, sizeof(buf), 100), TAG, "write");

    ESP_LOGI(TAG, "set to %04d-%02d-%02d %02d:%02d:%02d UTC",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
    return ESP_OK;
}

esp_err_t rtc_pcf85063_is_valid(bool *valid)
{
    ESP_RETURN_ON_FALSE(valid && s_dev, ESP_ERR_INVALID_ARG, TAG, "bad arg");

    uint8_t sec = 0;
    ESP_RETURN_ON_ERROR(read_regs(REG_SECONDS, &sec, 1), TAG, "read");

    *valid = !(sec & SEC_OS_FLAG);
    return ESP_OK;
}
