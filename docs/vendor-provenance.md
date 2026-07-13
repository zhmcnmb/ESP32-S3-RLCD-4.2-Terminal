# Vendor代码来源记录

本项目不整仓引入官方 [waveshareteam/ESP32-S3-RLCD-4.2](https://github.com/waveshareteam/ESP32-S3-RLCD-4.2) demo仓库。通用库(LVGL / ESP-SR / esp_codec_dev)通过 ESP-IDF component manager 声明依赖获取；只有board级、registry里拿不到的特有代码（例如ST7305初始化寄存器序列）才按需摘取到自建component中。

**规则**：每次从官方demo摘取代码片段，必须在下表登记来源，方便后续追溯/升级/排查license问题。

| 摘取日期 | 摘取内容 | 来源路径(官方仓库) | 来源commit | 放入本项目位置 |
|---|---|---|---|---|
| 2026-07-13 | ST7305初始化寄存器序列、横屏像素打包公式(每字节2×4像素交错位序)、全屏刷新窗口(0x2A:0x12-0x2A / 0x2B:0x00-0xC7)、SPI参数(10MHz/mode0/SPI3) | `02_Example/ESP-IDF/08_LVGL_V8_Test/components/port_bsp/display_bsp.cpp` | b2ca44c | `firmware/components/display_st7305/` (C重写) |
| 2026-07-13 | SD卡接口确认：SDMMC外设1-bit模式 CLK=GPIO38 CMD=GPIO21 D0=GPIO39（非SPI，与显示屏无总线冲突） | `02_Example/ESP-IDF/06_SD_Card/components/port_bsp/sdcard_bsp.h` 构造函数默认参数 | b2ca44c | `docs/hardware-pinmap.md` 登记 |
