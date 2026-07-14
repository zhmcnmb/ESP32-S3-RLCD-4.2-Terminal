# ESP32-S3-RLCD-4.2 多功能桌面终端

基于 [Waveshare ESP32-S3-RLCD-4.2](https://docs.waveshare.net/ESP32-S3-RLCD-4.2) 开发板的固件项目。

## 硬件

- 主控：ESP32-S3-WROOM-1-N16R8（双核240MHz，16MB Flash，8MB PSRAM）
- 屏幕：4.2寸全反射屏(RLCD)，ST7305驱动，300×400
- 音频：ES8311(DAC) + ES7210(4通道ADC) 双麦克风阵列，支持回声消除
- 传感器：SHTC3(温湿度) + PCF85063(RTC，独立电池)
- 存储：Micro SD卡槽
- 网络：2.4GHz WiFi + BLE5

详细引脚/总线分配见 [`docs/hardware-pinmap.md`](docs/hardware-pinmap.md)。

## 功能规划

1. ✅ 低功耗桌面时钟 + 天气显示（和风天气，P0-P3已完成并真机验证）
2. 语音交互（唤醒词 + LLM对话，P5未开始）
3. ✅ 环境监测记录（温湿度 + SD卡打点趋势，P2已完成并真机验证）
4. ✅ Claude Code 用量额度显示（需配合 `bridge/` 本地桥接服务，P4已完成并真机验证）

完整分阶段路线图见 [`CLAUDE.md`](CLAUDE.md)。

## 快速开始

```bash
# 需要先安装 ESP-IDF: https://github.com/espressif/esp-idf
cd firmware
idf.py set-target esp32s3
idf.py build flash monitor
```

## 目录结构

```
.
├── docs/           # 硬件引脚表、vendor代码来源记录、开发规范
├── firmware/       # ESP-IDF固件工程
└── bridge/         # Mac本地桥接服务：Claude用量+天气代理，已实现(P3+P4)
```
