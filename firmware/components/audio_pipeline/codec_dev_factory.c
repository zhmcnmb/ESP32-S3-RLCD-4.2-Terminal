/*
 * codec_dev_factory.c — ES7210 / ES8311 esp_codec_dev 实例构造。
 * 与 audio_pipeline.c 共享一个组件目录，对外只有 audio_pipeline.h。
 *
 * I2C 控制接口使用 board_i2c_bus() 返回的总线 handle（非 legacy port 号），
 * 对应 esp_codec_dev v1.2.0+ 新增的 bus_handle 字段，兼容 IDF >=5.3 新驱动。
 *
 * I2C 地址采用各 codec 默认宏（8-bit 移位形式，与头文件定义一致）：
 *   ES8311_CODEC_DEFAULT_ADDR = 0x30（7-bit 0x18，见 pinmap）
 *   ES7210_CODEC_DEFAULT_ADDR = 0x80（7-bit 0x40，见 pinmap）
 */

#include "codec_dev_factory.h"

#include "esp_log.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "es7210_adc.h"

#include "board_rlcd42.h"
#include "audio_pipeline.h"

static const char *TAG = "codec_factory";

/* 复用 board_rlcd42 初始化的 I2C 总线，构造 codec 控制接口。失败返回 NULL。 */
static const audio_codec_ctrl_if_t *create_i2c_ctrl(uint8_t addr_shifted)
{
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (bus == NULL) {
        ESP_LOGE(TAG, "I2C 总线未初始化（需先调用 board_init）");
        return NULL;
    }
    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr       = addr_shifted,
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl 失败 addr=0x%02X", addr_shifted);
    }
    return ctrl;
}

esp_codec_dev_handle_t ap_create_es8311(i2s_chan_handle_t tx_handle,
                                        const audio_codec_gpio_if_t *gpio_if)
{
    const audio_codec_ctrl_if_t *ctrl = create_i2c_ctrl(ES8311_CODEC_DEFAULT_ADDR);
    if (ctrl == NULL) return NULL;

    /* PA_EN 由 audio_pipeline 用普通 GPIO 手动管理，此处不交给 es8311 的 pa_pin。
     * hw_gain 决定 vol% → DAC 寄存器的映射，不设则按最保守值导致声音极小(实测无声)。
     * pa_voltage/codec_dac_voltage 为板级典型值，pa_gain 取功放典型增益 */
    es8311_codec_cfg_t codec_cfg = {
        .ctrl_if    = ctrl,
        .gpio_if    = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin     = -1,
        .use_mclk   = true,
        .hw_gain    = {
            .pa_voltage       = 5.0f,
            .codec_dac_voltage = 3.3f,
            .pa_gain          = 6.0f,
        },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&codec_cfg);
    if (codec_if == NULL) {
        ESP_LOGE(TAG, "es8311_codec_new 失败");
        return NULL;
    }

    audio_codec_i2s_cfg_t i2s_cfg = { .tx_handle = tx_handle };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (data_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data(tx) 失败");
        return NULL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec_if,
        .data_if  = data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
    };
    esp_codec_dev_handle_t dev = esp_codec_dev_new(&dev_cfg);
    if (dev == NULL) {
        ESP_LOGE(TAG, "esp_codec_dev_new(es8311) 失败");
        return NULL;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = AUDIO_PIPELINE_SAMPLE_RATE,
        .channel         = 1,
        .bits_per_sample = 16,
    };
    int rc = esp_codec_dev_open(dev, &fs);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open(es8311) 失败: %d", rc);
        esp_codec_dev_delete(dev); /* close对未open的dev无意义，delete才回收对象 */
        return NULL;
    }
    esp_codec_dev_set_out_vol(dev, 100);
    return dev;
}

esp_codec_dev_handle_t ap_create_es7210(i2s_chan_handle_t rx_handle)
{
    const audio_codec_ctrl_if_t *ctrl = create_i2c_ctrl(ES7210_CODEC_DEFAULT_ADDR);
    if (ctrl == NULL) return NULL;

    es7210_codec_cfg_t codec_cfg = {
        .ctrl_if       = ctrl,
        .master_mode   = false,
        .mic_selected  = ES7210_SEL_MIC1,
    };
    const audio_codec_if_t *codec_if = es7210_codec_new(&codec_cfg);
    if (codec_if == NULL) {
        ESP_LOGE(TAG, "es7210_codec_new 失败");
        return NULL;
    }

    audio_codec_i2s_cfg_t i2s_cfg = { .rx_handle = rx_handle };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (data_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data(rx) 失败");
        return NULL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec_if,
        .data_if  = data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
    };
    esp_codec_dev_handle_t dev = esp_codec_dev_new(&dev_cfg);
    if (dev == NULL) {
        ESP_LOGE(TAG, "esp_codec_dev_new(es7210) 失败");
        return NULL;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = AUDIO_PIPELINE_SAMPLE_RATE,
        .channel         = 1,
        .bits_per_sample = 16,
    };
    int rc = esp_codec_dev_open(dev, &fs);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open(es7210) 失败: %d", rc);
        esp_codec_dev_delete(dev);
        return NULL;
    }
    esp_codec_dev_set_in_gain(dev, 37.5f); /* ES7210 PGA 上限，远场拾音需要最大模拟增益 */
    return dev;
}
