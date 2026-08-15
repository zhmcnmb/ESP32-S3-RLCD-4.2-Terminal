#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SHTC3 温湿度传感器 (I2C 0x70)。依赖 board_init() 已完成。 */

typedef struct {
    float temperature; /* 摄氏度 */
    float humidity;    /* 相对湿度 % */
} shtc3_reading_t;

esp_err_t shtc3_init(void);

/* 单次测量(内部含唤醒-测量-休眠时序，约15ms阻塞) */
esp_err_t shtc3_read(shtc3_reading_t *out);

#ifdef __cplusplus
}
#endif
