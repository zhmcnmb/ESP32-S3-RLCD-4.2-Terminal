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
| `ui_pages` | `page_home`(时钟/日期/温湿度/网络状态)、`page_env_monitor`(温湿度趋势折线图)、`page_quota`(Claude用量5h/7d窗口)、`page_weather`(和风天气实况)。`page_diag`(硬件自检)已从`app_main.c`注册中移除、不再出现在设备实机翻页里，但文件保留作为**新页面参照范例**(见文件头注释) |
| `sensor_shtc3` | SHTC3温湿度读取 |
| `rtc_pcf85063` | RTC读写(BCD转换)、掉电检测。**符号必须用全前缀**，见Gotchas |
| `net_manager` | WiFi STA连接、断线自动重连 |
| `time_service` | 启动从RTC恢复时间，联网后SNTP校时并写回RTC |
| `storage_sd` | SD卡挂载(SDMMC 1-bit + FATFS)，`storage_sd_append_line()`按相对路径追加写 |
| `battery` | 电池电压ADC(GPIO4,3倍分压,eFuse校准)，电量百分比估算 |
| `env_logger` | 后台任务60s采样SHTC3，环形缓冲(1小时窗口)+CSV落盘(经`storage_sd`，SD卡可选) |
| `claude_quota_client` | mDNS解析桥接服务主机名+HTTP轮询`/api/quota/compact`+cJSON解析，60s周期(登记于coding-standards第6节) |
| `weather_client` | mDNS解析桥接服务主机名+HTTP轮询`/api/weather`+cJSON解析，15分钟周期。**不直连QWeather**，原因见下方"P3 天气代理"一节 |

**中文字库**：`tools/charset.txt` 维护字符集 → `python3 tools/gen_font.py` 生成 `font_cn16.c`。
源码字符串字面量里的中文会被`gen_font.py`自动扫描收录，**新增文案后只需重跑脚本**；只有运行时才出现、源码里没有字面量的文案(如天气API返回的现象词、风向)才需要手工写进`charset.txt`。缺字时屏幕显示空心框。当前231字约7.7KB。

**待实现**

| Component | 职责 | Phase |
|---|---|---|
| `audio_pipeline` / `wakeword` / `voice_assistant` | ES8311/ES7210音频、ESP-SR唤醒、LLM对话 | P5 |

**新增页面的做法**：在 `ui_pages/pages/` 下照抄 `page_diag.c` 的结构，实现 `ui_page_t`，在 `ui_pages.h` 暴露 getter，在 `app_main.c` 注册一行。**不要改 ui_manager**。

## P4 用量数据源（重要设计决定）

**开发环境本机用的是 OMP 框架（`omp` CLI）跑 Claude 订阅，不是官方 `claude` CLI**，因此`bridge/`原计划"解析`~/.claude/projects/**/*.jsonl`做近似估算"的方案**已废弃**。

实测确认：`omp usage --provider anthropic --json` 直接返回 Anthropic 官方(未文档化)OAuth用量接口的**权威**数据——`anthropic:5h`(Claude 5 Hour)与`anthropic:7d`(Claude 7 Day)两个窗口的`usedFraction`+`resetsAt`，与Claude Code自带`/usage`背后是同一接口。这份数据是**账号级**的，不区分客户端(官方`claude` CLI、OMP、claude.ai网页都算同一份用量)，所以不需要本机解析任何会话日志，也不需要"近似估算"标注。OMP自身会把结果写入`~/.omp/agent/agent.db`的`usage_history`表(带`resets_at`/`used_fraction`)做缓存。

`bridge/`桥接服务改为：定时(如60s)`subprocess`调用`omp usage --provider anthropic --json`并解析，替代原jsonl增量解析方案。详见`bridge/README.md`。

## P3 天气代理（重要设计决定）

`weather_client` **不直连QWeather**，改走`bridge/`代理转发，和P4 Claude用量走同一个桥接服务。原因：实测确认QWeather新版API Host（项目专属域名）**不管请求方Accept-Encoding怎么设都强制gzip压缩响应**（试过`--compressed`和`Accept-Encoding: identity`，服务端一律无视）。ESP-IDF的`esp_http_client`不做自动gzip解压，要在ESP32上解得接ROM里阉割版的miniz `tinfl`函数手动解，还要为HTTPS额外引入`mbedtls`+`esp_crt_bundle`整套证书链——工程量不小、固件体积显著增大(实测：直连方案73%可用空间→改代理后不用HTTPS/gzip，涨回来了)。

Python标准库`gzip.decompress()`一行代码解决，所以`bridge/server.py`新增`fetch_weather()`+`/api/weather`接口代理转发，ESP32端`weather_client`复用`claude_quota_client`已验证过的mDNS发现机制走局域网**明文HTTP**。副作用是好事：和风天气API Key不再进入烧录进设备的固件（`secrets.h`），只留在Mac本地`bridge/config.local.json`（已`.gitignore`），比直连方案密钥暴露面更小。详见`bridge/server.py`头注释与`bridge/README.md`。

## 已知风险 / 待确认事项

- ~~SD卡引脚~~ 已确认：SDMMC外设1-bit模式，CLK=38/CMD=21/D0=39，与显示SPI无冲突（详见`docs/hardware-pinmap.md`）
- ~~ST7305初始化序列~~ 已从官方`08_LVGL_V8_Test`摘取（登记于`docs/vendor-provenance.md`）
- **ST7305像素排布特殊**：横屏400×300时每字节打包2×4像素、交错位序，`index=(x/2)*75+(299-y)/4`，帧缓冲15000字节；不能按常规行连续1bpp处理，LVGL接入时flush回调需逐像素转换
- WiFi凭证/API Key等密钥一律不进仓库，开发阶段用`.gitignore`排除的本地头文件占位，正式方案后续再定
- ~~烧录卡在"No serial data received"~~ 已解决：原因是USB连接会话失效(设备节点`/dev/cu.usbmodem101`残留但无实际数据通路)，**重新插拔USB后设备换了新节点`/dev/cu.usbmodem1101`即可正常烧录**——纯软件侧(检查`ioreg`枚举、DTR/RTS复位脉冲)排查不出这类情况，本质是物理连接层，遇到同症状先重插USB
- **多任务并发读同一I2C设备会导致时序错乱**：`env_logger_task`(60s周期)与`ui_task`(page_home on_enter)同时调用`shtc3_read()`时，无锁的多步命令时序(唤醒→测量→读取→休眠)会被另一任务的命令打断，导致`ESP_ERR_INVALID_RESPONSE`。已修复：`sensor_shtc3`内部加`SemaphoreHandle_t`互斥锁串行化整个读取时序。**新增任何会被多任务调用的I2C多步时序组件都要照此加锁**，不能假设ESP-IDF I2C驱动的单次事务线程安全等于设备协议时序安全
- **ASCII点阵字体只收录`0x20~0x5A`**（空格到大写Z）：`st7305_draw_text`/`draw_utf8`里的ASCII部分超出这个范围(如`~`0x7E、小写字母)会画成空心框而不是报错，很容易漏看。已踩过一次：`page_env_monitor`用`~`表示数值范围，画出来是空心框，改成`-`(0x2D，在范围内)解决。**新增展示字符串前检查有没有用到`0x5A`以外的ASCII符号**

## 分阶段路线图

| Phase | 内容 | 完成标准 |
|---|---|---|
| P0 | 项目初始化 ✅已完成并真机验证 | 固件烧录成功、串口hello心跳正常；实测确认：16MB Flash(QIO@80MHz)、8MB Octal PSRAM、双核240MHz |
| P1 | 板级点亮 ✅已完成并真机验证 | I2C扫描4/4器件在线、ST7305屏幕刷新成功、KEY/BOOT按键正常；页面框架(ui_manager+page_diag)跑通 |
| P2 | 环境监测MVP：RTC+SHTC3+SD打点+趋势页面 ✅已完成并真机验证 | CSV持续写入(2个采样周期0错误)，SD卡格式化后持久挂载，断电重启RTC不丢时间 |
| P3 | 联网时钟天气：WiFi+NTP+mDNS+和风天气 ✅已完成并真机验证 | 显示正确本地时间与指定城市天气(bridge日志确认ESP32多次成功轮询`/api/weather`拿到200，`page_weather`展示"阴/东风0级/29℃/89%"等真实数据) |
| P4 | Claude额度显示：Mac桥接服务+ESP32轮询+额度页面 ✅已完成并真机验证 | 屏幕数字与手动核对吻合(bridge日志确认ESP32多次成功轮询`/api/quota/compact`拿到200) |
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
