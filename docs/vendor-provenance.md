# Vendor代码来源记录

本项目不整仓引入官方 [waveshareteam/ESP32-S3-RLCD-4.2](https://github.com/waveshareteam/ESP32-S3-RLCD-4.2) demo仓库。通用库(LVGL / ESP-SR / esp_codec_dev)通过 ESP-IDF component manager 声明依赖获取；只有board级、registry里拿不到的特有代码（例如ST7305初始化寄存器序列）才按需摘取到自建component中。

**规则**：每次从官方demo摘取代码片段，必须在下表登记来源，方便后续追溯/升级/排查license问题。

| 摘取日期 | 摘取内容 | 来源路径(官方仓库) | 来源commit | 放入本项目位置 |
|---|---|---|---|---|
| 2026-07-13 | ST7305初始化寄存器序列、横屏像素打包公式(每字节2×4像素交错位序)、全屏刷新窗口(0x2A:0x12-0x2A / 0x2B:0x00-0xC7)、SPI参数(10MHz/mode0/SPI3) | `02_Example/ESP-IDF/08_LVGL_V8_Test/components/port_bsp/display_bsp.cpp` | b2ca44c | `firmware/components/display_st7305/` (C重写) |
| 2026-07-13 | SD卡接口确认：SDMMC外设1-bit模式 CLK=GPIO38 CMD=GPIO21 D0=GPIO39（非SPI，与显示屏无总线冲突） | `02_Example/ESP-IDF/06_SD_Card/components/port_bsp/sdcard_bsp.h` 构造函数默认参数 | b2ca44c | `docs/hardware-pinmap.md` 登记 |
| 2026-07-16 | ESP-SR AFE初始化流程(`esp_srmodel_init` → `afe_config_init` → `esp_afe_handle_from_config` → `create_from_config`)、AFE feed/fetch循环、WakeNet检测结果处理 | `esp-skainet/examples/wake_word_detection/afe/main/main.c` | master (7ff63a7) | `firmware/components/wakeword/wakeword.c` |
| 2026-07-16 | audio_pipeline 的 ES8311/ES7210 codec 构造流程(`audio_codec_new_i2c_ctrl` 用 `bus_handle` 字段、`es8311_codec_new` / `es7210_codec_new` 配置、`esp_codec_dev_open` 固定 16kHz mono 16bit、PA_EN 由调用方 GPIO 管理的模式)、I2S 双工通道创建(`i2s_new_channel` + `i2s_channel_init_std_mode`) | `esp_codec_dev/test_apps/codec_dev_test/main/test_board.c` + README 用法示例 | v1.5.9 | `firmware/components/audio_pipeline/audio_pipeline.c` + `codec_dev_factory.c` |
| 2026-07-16 | MultiNet持续监听模式(`esp_srmodel_filter`+`esp_mn_handle_from_name`+`multinet->create`+`esp_mn_commands_update_from_sdkconfig`+`detect()`状态机DETECTING/DETECTED/TIMEOUT处理)——本项目去掉了官方示例里WakeNet先行门控的`wakeup_flag`，改为不经WakeNet门控、直接持续调用`multinet->detect()`(唤醒词"贾维斯"无官方WakeNet模型，详见docs/voice-agent-design.md第9.4节决策) | `esp-skainet/examples/cn_speech_commands_recognition/main/main.c` | master | `firmware/components/wakeword/wakeword.c` |