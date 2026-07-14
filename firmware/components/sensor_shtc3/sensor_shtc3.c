#include "sensor_shtc3.h"

#include "board_rlcd42.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "shtc3";

/* SHTC3 命令 (datasheet)。
 * 用不带clock-stretching的测量模式，由主机侧延时等待转换完成 */
#define CMD_WAKEUP        0x3517
#define CMD_SLEEP         0xB098
#define CMD_MEAS_T_FIRST  0x7866  /* 普通模式，温度优先，无时钟拉伸 */

/* 唤醒需240us。注意不能用 vTaskDelay: FreeRTOS默认tick=100Hz，
 * pdMS_TO_TICKS(1) 会算成 0 tick 完全不延时，导致芯片未醒就收命令而NACK */
#define WAKEUP_DELAY_US   500
/* 普通模式转换最长12.1ms，取20ms留余量(tick粒度10ms，20ms=2tick) */
#define MEASURE_DELAY_MS  20

static i2c_master_dev_handle_t s_dev = NULL;
static SemaphoreHandle_t s_lock = NULL; /* 串行化多步时序，防止并发调用方(ui_task/env_logger_task)交错命令 */

static esp_err_t send_cmd(uint16_t cmd)
{
    uint8_t buf[2] = { cmd >> 8, cmd & 0xFF };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

esp_err_t shtc3_init(void)
{
    ESP_RETURN_ON_FALSE(board_i2c_bus(), ESP_ERR_INVALID_STATE, TAG, "board not inited");

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_I2C_ADDR_SHTC3,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(board_i2c_bus(), &cfg, &s_dev), TAG, "add dev");

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex创建失败");

    /* 唤醒后立即休眠，确认器件应答正常 */
    ESP_RETURN_ON_ERROR(send_cmd(CMD_WAKEUP), TAG, "wakeup");
    esp_rom_delay_us(WAKEUP_DELAY_US);
    ESP_RETURN_ON_ERROR(send_cmd(CMD_SLEEP), TAG, "sleep");

    ESP_LOGI(TAG, "init OK");
    return ESP_OK;
}

/* 实际读取时序，调用方须持有 s_lock */
static esp_err_t read_locked(shtc3_reading_t *out)
{
    ESP_RETURN_ON_ERROR(send_cmd(CMD_WAKEUP), TAG, "wakeup");
    esp_rom_delay_us(WAKEUP_DELAY_US);

    ESP_RETURN_ON_ERROR(send_cmd(CMD_MEAS_T_FIRST), TAG, "measure");
    vTaskDelay(pdMS_TO_TICKS(MEASURE_DELAY_MS));

    /* 6字节: 温度MSB/LSB/CRC + 湿度MSB/LSB/CRC */
    uint8_t buf[6] = { 0 };
    esp_err_t err = i2c_master_receive(s_dev, buf, sizeof(buf), 100);
    send_cmd(CMD_SLEEP); /* 无论成败都回休眠，降低自热 */
    ESP_RETURN_ON_ERROR(err, TAG, "receive");

    /* datasheet 转换公式 */
    uint16_t raw_t = (buf[0] << 8) | buf[1];
    uint16_t raw_h = (buf[3] << 8) | buf[4];
    out->temperature = -45.0f + 175.0f * (float)raw_t / 65535.0f;
    out->humidity = 100.0f * (float)raw_h / 65535.0f;

    return ESP_OK;
}

esp_err_t shtc3_read(shtc3_reading_t *out)
{
    ESP_RETURN_ON_FALSE(out && s_dev, ESP_ERR_INVALID_ARG, TAG, "bad arg");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = read_locked(out);
    xSemaphoreGive(s_lock);
    return err;
}
