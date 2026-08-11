#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 板级唯一引脚定义与总线初始化。
 * 全项目的GPIO/总线init只允许出现在这里，其他组件只拿handle。
 * 引脚权威来源: docs/hardware-pinmap.md
 */

/* 显示屏 ST7305 (SPI3) */
#define BOARD_LCD_DC_PIN     GPIO_NUM_5
#define BOARD_LCD_CS_PIN     GPIO_NUM_40
#define BOARD_LCD_SCK_PIN    GPIO_NUM_11
#define BOARD_LCD_MOSI_PIN   GPIO_NUM_12
#define BOARD_LCD_RST_PIN    GPIO_NUM_41
#define BOARD_LCD_SPI_HOST   SPI3_HOST
#define BOARD_LCD_WIDTH      400   /* 横屏 */
#define BOARD_LCD_HEIGHT     300

/* I2C共享总线: SHTC3/ES8311/ES7210/PCF85063 */
#define BOARD_I2C_SDA_PIN    GPIO_NUM_13
#define BOARD_I2C_SCL_PIN    GPIO_NUM_14
#define BOARD_I2C_ADDR_SHTC3     0x70
#define BOARD_I2C_ADDR_ES8311    0x18
#define BOARD_I2C_ADDR_ES7210    0x40
#define BOARD_I2C_ADDR_PCF85063  0x51

/* SD卡 (SDMMC外设 1-bit模式，非SPI) */
#define BOARD_SD_CLK_PIN     GPIO_NUM_38
#define BOARD_SD_CMD_PIN     GPIO_NUM_21
#define BOARD_SD_D0_PIN      GPIO_NUM_39

/* I2S音频 */
#define BOARD_I2S_LRCLK_PIN  GPIO_NUM_45
#define BOARD_I2S_BCLK_PIN   GPIO_NUM_9
#define BOARD_I2S_MCLK_PIN   GPIO_NUM_16
#define BOARD_I2S_DOUT_PIN   GPIO_NUM_8
#define BOARD_I2S_DIN_PIN    GPIO_NUM_10
#define BOARD_PA_EN_PIN      GPIO_NUM_46

/* 按键(低电平按下) / 电池ADC */
#define BOARD_BTN_BOOT_PIN   GPIO_NUM_0
#define BOARD_BTN_KEY_PIN    GPIO_NUM_18
#define BOARD_BAT_ADC_PIN    GPIO_NUM_4

/* 初始化I2C总线、显示SPI总线、按键GPIO。幂等，重复调用直接返回。 */
esp_err_t board_init(void);

/* 已初始化的I2C总线handle，board_init()之前调用返回NULL */
i2c_master_bus_handle_t board_i2c_bus(void);

/* 按键状态，true=按下 */
bool board_btn_boot_pressed(void);
bool board_btn_key_pressed(void);

#ifdef __cplusplus
}
#endif
