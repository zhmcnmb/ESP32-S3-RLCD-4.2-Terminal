# CLAUDE.md

本文件是给未来 Claude Code 会话看的权威上下文，避免重复调研。

**写代码前必读 [`docs/coding-standards.md`](docs/coding-standards.md)** —— 分层依赖规则、文件体积红线、错误处理、页面接口契约都是强制约定。硬件引脚的唯一权威来源是 [`docs/hardware-pinmap.md`](docs/hardware-pinmap.md)。

## 项目是什么

Waveshare ESP32-S3-RLCD-4.2 开发板固件，目标是打造一个桌面终端，集成：低功耗时钟+天气、语音交互(唤醒词+LLM对话)、环境监测记录(温湿度+SD打点)、Claude Code用量额度显示。

## 框架选型：ESP-IDF（非Arduino）

理由：语音交互依赖 ESP-SR 原生组件；官方仓库自带的 XiaoZhi(小智) demo 是 ESP-IDF 实现，已验证过本板的 ES8311/ES7210 音频链路；多外设(显示+音频+网络+SD+RTC)并发场景下 FreeRTOS 任务模型更可控。

## 官方参考资源

- 文档：https://docs.waveshare.net/ESP32-S3-RLCD-4.2
- 官方demo仓库：https://github.com/waveshareteam/ESP32-S3-RLCD-4.2
  - `02_Example/ESP-IDF/` 下按外设拆分了独立示例（`04_I2C_PCF85063` `05_I2C_SHTC3` `06_SD_Card` `07_Audio_Test` `08_LVGL_V8_Test` `09_LVGL_V9_Test` `11_U8G2_Test` 等）
  - `02_Example/XiaoZhi/XiaoZhiCode_V2.1.0` 是语音交互(Phase 5)的参考实现，**不直接继承其app层**，只参考音频链路配置

## Vendor策略

不整仓引入官方demo。通用库(lvgl / esp-sr / esp_codec_dev)走 ESP-IDF component manager 声明依赖；仅board级特有代码（如ST7305初始化寄存器序列，registry里拿不到的部分）按需摘取到自建component，摘取时必须在 `docs/vendor-provenance.md` 记录来源文件路径和上游commit。XiaoZhi代码体积大、耦合深，推迟到 Phase 5 才作为pinned submodule引入`vendor/xiaozhi/`，仅供只读参考。

## 固件Component现状与规划

依赖只能向下（详见规范第1节）：`main → ui_pages → ui_manager → 服务层 → 驱动层 → board_rlcd42`

**已实现**

| Component | 职责 |
|---|---|
| `board_rlcd42` | 唯一的引脚定义与总线init(I2C/SPI/按键)，暴露handle与I2C扫描 |
| `display_st7305` | ST7305反射屏驱动：1bpp帧缓冲、画点/矩形、ASCII 8×8字体、**中文16×16字库(UTF-8混排)** |
| `ui_manager` | 页面注册、导航、按键长短按判定、tick调度（ui_task） |
| `ui_pages` | `page_home`(时钟/日期/温湿度/网络状态)、`page_diag`(硬件自检，**新页面参照范例**) |
| `sensor_shtc3` | SHTC3温湿度读取 |
| `rtc_pcf85063` | RTC读写(BCD转换)、掉电检测。**符号必须用全前缀**，见Gotchas |
| `net_manager` | WiFi STA连接、断线自动重连 |
| `time_service` | 启动从RTC恢复时间，联网后SNTP校时并写回RTC |

**中文字库**：`tools/charset.txt` 维护字符集 → `python3 tools/gen_font.py` 生成 `font_cn16.c`。
**新增中文文案时必须把新字加进 charset.txt 并重跑脚本**，否则屏幕显示空心框。当前122字约4KB。

**待实现**

| Component | 职责 | Phase |
|---|---|---|
| `storage_sd` | SD卡挂载(SDMMC 1-bit + FATFS) | P2 |
| `env_logger` | 温湿度历史环形缓冲、CSV落盘、趋势页 | P2 |
| `weather_client` | 和风天气(QWeather)拉取+解析+缓存 | P3 |
| `claude_quota_client` | 轮询Mac桥接服务，解析用量JSON | P4 |
| `audio_pipeline` / `wakeword` / `voice_assistant` | ES8311/ES7210音频、ESP-SR唤醒、LLM对话 | P5 |

**新增页面的做法**：在 `ui_pages/pages/` 下照抄 `page_diag.c` 的结构，实现 `ui_page_t`，在 `ui_pages.h` 暴露 getter，在 `app_main.c` 注册一行。**不要改 ui_manager**。

## 已知风险 / 待确认事项

- ~~SD卡引脚~~ 已确认：SDMMC外设1-bit模式，CLK=38/CMD=21/D0=39，与显示SPI无冲突（详见`docs/hardware-pinmap.md`）
- ~~ST7305初始化序列~~ 已从官方`08_LVGL_V8_Test`摘取（登记于`docs/vendor-provenance.md`）
- **ST7305像素排布特殊**：横屏400×300时每字节打包2×4像素、交错位序，`index=(x/2)*75+(299-y)/4`，帧缓冲15000字节；不能按常规行连续1bpp处理，LVGL接入时flush回调需逐像素转换
- WiFi凭证/API Key等密钥一律不进仓库，开发阶段用`.gitignore`排除的本地头文件占位，正式方案后续再定

## 分阶段路线图

| Phase | 内容 | 完成标准 |
|---|---|---|
| P0 | 项目初始化 ✅已完成并真机验证 | 固件烧录成功、串口hello心跳正常；实测确认：16MB Flash(QIO@80MHz)、8MB Octal PSRAM、双核240MHz |
| P1 | 板级点亮 ✅已完成并真机验证 | I2C扫描4/4器件在线、ST7305屏幕刷新成功、KEY/BOOT按键正常；页面框架(ui_manager+page_diag)跑通 |
| P2 | 环境监测MVP：RTC+SHTC3+SD打点+趋势页面 | CSV持续写入，断电重启RTC不丢时间 |
| P3 | 联网时钟天气：WiFi+NTP+mDNS+和风天气 | 显示正确本地时间与指定城市天气 |
| P4 | Claude额度显示：Mac桥接服务+ESP32轮询+额度页面 | 屏幕数字与手动核对吻合 |
| P5 | 语音交互：音频链路+ESP-SR唤醒词+LLM对话 | 唤醒可靠，问答闭环完整 |
| P6 | 打磨：页面导航UX、功耗策略、看门狗、稳定性 | 长时间稳定运行 |

## 构建

ESP-IDF **v6.0.2** 装在 `~/esp/esp-idf`，工具链在 `~/.espressif`。cmake/ninja 等前置依赖装在 conda 的 `esp-idf-tools` 环境（刻意不用 base，避免污染）。

**每个新终端必须先激活环境，否则 `idf.py` 不在 PATH：**

```bash
conda activate esp-idf-tools
source ~/esp/esp-idf/export.sh
```

构建与烧录（板子是 ESP32-S3 内置 USB-JTAG/CDC，串口名形如 `/dev/cu.usbmodem*`，**具体数字随USB口变化**，用 `ls /dev/cu.usbmodem*` 探测）：

```bash
cd firmware
idf.py build
idf.py -p $(ls /dev/cu.usbmodem*) flash
```

## Gotchas

- **改了 `sdkconfig.defaults` 必须 `rm -f firmware/sdkconfig` 再 build**，否则已生成的 `sdkconfig` 会覆盖默认值，改动静默失效
- **macOS 没有 `timeout` 命令**，非交互抓串口日志用 ESP-IDF venv 里的 pyserial（conda 环境里没装 pyserial）：`~/.espressif/python_env/idf6.0_py3.11_env/bin/python`，用 `setRTS` 脉冲复位板子后读 `/dev/cu.usbmodem1101`
- `idf.py monitor` 是交互式的，脚本化场景改用上面的 pyserial 方式
- ESP-IDF 浅克隆时 `--recursive` 可能漏拉子模块（本次 unity 就没拉全，cmake 报 "Include directory ... is not a directory"），补救：`cd ~/esp/esp-idf && git submodule update --init --recursive --depth 1`
- **绿灯常亮 ≠ 板子在运行**：绿灯是CHG充电指示，PWR键硬件关机后绿灯仍亮但主控断电、USB不枚举。串口消失先按PWR键开机再排查线材
- **RLCD反射屏没有背光**，不存在"点亮"状态；屏幕有无内容取决于固件是否驱动了ST7305
- **组件符号必须用全前缀**：曾用 `rtc_init` 与 ESP-IDF 内部 `esp_hw_support/rtc_init` 链接冲突，改名 `rtc_pcf85063_init` 才通过。短前缀(`rtc_`/`net_`/`ui_`)有撞名风险
- **默认1MB app分区不够用**：光WiFi协议栈就占83%。已改自定义 `partitions.csv`（app 4MB + 语音模型8MB + 存储4MB）
- **Bash工具的 `cd` 不跨调用保持**，`sed -i` 等命令要用绝对路径，否则静默不生效
