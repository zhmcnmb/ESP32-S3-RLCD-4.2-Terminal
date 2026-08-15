#include "battery.h"

#include "board_rlcd42.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "battery";

/* GPIO4 = ADC1_CH3 */
#define BAT_ADC_UNIT     ADC_UNIT_1
#define BAT_ADC_CHANNEL  ADC_CHANNEL_3
/* 硬件是3倍分压 */
#define VOLTAGE_DIVIDER  3.0f
#define SAMPLE_COUNT     16

/* 18650锂电池电压区间。放电曲线非线性，这里用分段线性近似 */
#define BAT_FULL_V       4.15f
#define BAT_EMPTY_V      3.30f
/* 高于此电压说明USB在供电/充电，而非纯电池放电 */
#define BAT_CHARGING_V   4.25f

static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;

esp_err_t battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BAT_ADC_UNIT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "adc unit");

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,  /* 量程约0~3.1V，配合3倍分压覆盖电池全区间 */
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err = adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        ESP_LOGE(TAG, "adc channel");
        return err;
    }

    /* 校准曲线来自eFuse，没有校准数据时降级为裸读数(精度下降但仍可用) */
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .chan = BAT_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC无校准数据，电压读数精度下降");
        s_cali = NULL;
    }

    ESP_LOGI(TAG, "init OK (GPIO%d)", BOARD_BAT_ADC_PIN);
    return ESP_OK;
}

static int percent_from_voltage(float v)
{
    if (v >= BAT_FULL_V) {
        return 100;
    }
    if (v <= BAT_EMPTY_V) {
        return 0;
    }
    return (int)((v - BAT_EMPTY_V) / (BAT_FULL_V - BAT_EMPTY_V) * 100.0f);
}

esp_err_t battery_read(battery_status_t *out)
{
    ESP_RETURN_ON_FALSE(out && s_adc, ESP_ERR_INVALID_STATE, TAG, "未初始化");

    int sum_mv = 0;
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        int raw = 0;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw), TAG, "adc read");

        int mv = raw;
        if (s_cali) {
            ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali, raw, &mv), TAG, "cali");
        }
        sum_mv += mv;
    }

    float pin_voltage = (float)sum_mv / SAMPLE_COUNT / 1000.0f;
    out->voltage = pin_voltage * VOLTAGE_DIVIDER;
    out->charging = (out->voltage >= BAT_CHARGING_V);
    out->percent = percent_from_voltage(out->voltage);
    return ESP_OK;
}
