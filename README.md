# ESP32-S3-RLCD-4.2 多功能桌面终端

基于 [Waveshare ESP32-S3-RLCD-4.2](https://docs.waveshare.net/ESP32-S3-RLCD-4.2) 开发板的 ESP-IDF 固件项目：反射屏桌面终端 + 端侧语音智能体。

## 硬件

- 主控：ESP32-S3-WROOM-1-N16R8（双核240MHz，16MB Flash，8MB PSRAM）
- 屏幕：4.2寸全反射屏(RLCD)，ST7305驱动，300×400
- 音频：ES8311(DAC) + ES7210(4通道ADC) 双麦克风阵列，支持回声消除
- 传感器：SHTC3(温湿度) + PCF85063(RTC，独立电池)
- 存储：Micro SD卡槽
- 网络：2.4GHz WiFi + BLE5

## 功能

- 低功耗桌面时钟（PCF85063 RTC 独立电池走时）
- 天气显示（和风天气）
- 环境监测与趋势记录（SHTC3 温湿度 + SD 卡打点）
- 语音交互：按键 PTT 录音 → 云端 ASR/LLM → 流式 TTS 播报（SiliconFlow / DeepSeek）
- 端侧智能体：板载记忆、定时提醒、免 Key Web 检索（Bing 搜索 + 白名单正文抓取）
- 按键配网：扫描/列表/密码输入全流程，凭据写入 NVS

## 按键

| 按键 | 短按 | 长按 |
|---|---|---|
| KEY | 下一页 | 上一页 |
| BOOT | 按住说话(PTT) / 配网确认 | 取消 |

WiFi 页短按 BOOT 进入配网流程后，KEY/BOOT 转为配网操作，逐层退出后恢复翻页。

## 快速开始

```bash
# 需要先安装 ESP-IDF v6.x: https://github.com/espressif/esp-idf
cd firmware
idf.py set-target esp32s3
cp main/secrets.h.example main/secrets.h   # 填入 WiFi 与云服务凭据
idf.py build flash monitor
```

`secrets.h` 已被 `.gitignore` 排除，不会进仓库。设备端配网成功后凭据写入 NVS，后续不再依赖 `secrets.h` 中的 WiFi 配置。

## 目录结构

```
├── firmware/       # ESP-IDF 固件工程
│   ├── main/       # 入口与 secrets.h 凭据模板
│   └── components/ # 功能组件（显示/音频/网络/agent/记忆/调度等）
└── tools/          # 中文字库生成等开发工具
```

## 许可证

[MIT](LICENSE)
