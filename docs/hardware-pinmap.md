# 硬件引脚 / 总线权威表

本表是全项目唯一权威引脚参考，`board_rlcd42` component的实现必须与此表一致，若有出入以实际连板测试为准并回来更新本表。

来源：官方仓库 `02_Example/ESP-IDF/06_SD_Card/main/user_config.h` 源码实测 + ESPHome社区配置(`devices.esphome.io`)交叉验证。

## 显示屏（ST7305，300×400，SPI，无背光反射屏）

| 信号 | GPIO |
|---|---|
| DC (Data/Command) | GPIO5 |
| CS (片选) | GPIO40 |
| CLK (SPI时钟) | GPIO11 |
| MOSI | GPIO12 |
| RESET | GPIO41 |
| TE/触发 (Trigger) | GPIO6 |

## I2C 共享总线（SHTC3 / ES8311 / ES7210 / PCF85063 共用）

| 信号 | GPIO |
|---|---|
| SDA | GPIO13 |
| SCL | GPIO14 |

| 设备 | 7-bit地址 | 功能 |
|---|---|---|
| SHTC3 | 0x70 | 温湿度传感器 |
| ES8311 | 0x18 | 音频DAC/功放驱动 |
| ES7210 | 0x40 | 4通道音频ADC |
| PCF85063 | 0x51 | RTC实时时钟 |

四地址互不冲突，可安全共用一条I2C总线。总线初始化必须唯一由`board_rlcd42`完成，其余component只申请自己的dev handle。

## I2S 音频

| 信号 | GPIO |
|---|---|
| LRCLK | GPIO45 |
| BCLK | GPIO9 |
| MCLK | GPIO16 |
| DOUT (喇叭输出) | GPIO8 |
| DIN (麦克风输入) | GPIO10 |
| PA_EN (功放使能) | GPIO46 |

注意：GPIO46上电初期需保持已知电平，避免开机爆音；ES7210是4通道TDM，双麦+回采参考通道用于AEC，建议Phase 5直接沿用官方XiaoZhi demo已验证的时钟/TDM配置。

## 按键 / 电源 / 电池

| 信号 | GPIO | 备注 |
|---|---|---|
| BOOT键 | GPIO0 | 低电平触发，也是烧录强制下载脚(strapping pin) |
| KEY键 | GPIO18 | 低电平触发，自定义功能 |
| PWR键 | 硬件级电源开关 | 非GPIO控制 |
| 电池电压ADC | GPIO4 | ADC1，3倍分压，需`esp_adc_cal`校准 |

## USB 下载 / 串口（真机实测，2026-07-17）

Type-C 直接连接 ESP32-S3 原生 USB Serial/JTAG，用于烧录和日志；macOS 正常枚举时会显示
`Espressif USB JTAG/serial debug unit`，并由 CDC-ACM 驱动创建
`/dev/cu.usbmodem*`。后缀会变化，不能硬编码。

诊断顺序：

1. 先运行 `ioreg -p IOUSB -l -w 0`：没有 Espressif 子设备时，问题在 PWR 状态、数据线、
   直连端口或板载 Type-C 物理链路，尚未进入驱动和 ESP-IDF 层。
2. 有设备但无法确定串口名时，运行
   `ioreg -p IOService -n 'USB JTAG/serial debug unit' -r -l`，从
   `IOSerialBSDClient` 的 `IOCalloutDevice` 读取实际 `/dev/cu.usbmodem*` 路径。
3. 用 `stat /dev/cu.usbmodem*` 或 `idf.py -p <实际路径> flash` 验证；自动化环境中的
   glob 结果不能作为 `/dev` 特殊设备节点不存在的依据。

进入下载模式：按住 BOOT，再短按 PWR 开机，随后松开 BOOT。

## 存储（已确认，来源：官方`06_SD_Card`示例 sdcard_bsp.h，commit b2ca44c）

| 信号 | GPIO |
|---|---|
| SD CLK | GPIO38 |
| SD CMD | GPIO21 |
| SD D0 | GPIO39 |

SD卡走 **SDMMC外设 1-bit模式**（`esp_vfs_fat_sdmmc_mount`），不是SPI——与显示屏(SPI3)无总线冲突，之前担心的bus mutex问题不存在。

## 主控

ESP32-S3-WROOM-1-N16R8：双核Xtensa LX7 @240MHz，16MB Flash，8MB PSRAM（Octal模式），512KB SRAM，2.4GHz WiFi + BLE5。
