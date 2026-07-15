/*
 * 内部辅助：创建 ES7210 / ES8311 esp_codec_dev 实例。
 * 仅 audio_pipeline.c 使用，不暴露到 include/。
 */
#pragma once

#include "esp_codec_dev.h"
#include "driver/i2s_std.h"
#include "audio_codec_gpio_if.h"

/* 以当前 board_i2c_bus() 为控制总线创建 ES8311 播放设备（仅 DAC）。
 * 失败返回 NULL，不会留下半成品资源。*/
esp_codec_dev_handle_t ap_create_es8311(i2s_chan_handle_t tx_handle,
                                        const audio_codec_gpio_if_t *gpio_if);

/* 以当前 board_i2c_bus() 为控制总线创建 ES7210 录音设备（仅 MIC1）。
 * 失败返回 NULL。*/
esp_codec_dev_handle_t ap_create_es7210(i2s_chan_handle_t rx_handle);
