#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 18650电池电量监测。GPIO4 (ADC1_CH3)，硬件3倍分压。
 * 用ESP-IDF校准接口把原始读数转成真实电压，未校准的裸读数误差可达±10%。
 */

typedef struct {
    float voltage;   /* 电池电压(V) */
    int percent;     /* 估算电量 0~100 */
    bool charging;   /* USB供电中(电压高于满电阈值时判定) */
} battery_status_t;

esp_err_t battery_init(void);

/* 多次采样取均值，抑制ADC噪声 */
esp_err_t battery_read(battery_status_t *out);

#ifdef __cplusplus
}
#endif
