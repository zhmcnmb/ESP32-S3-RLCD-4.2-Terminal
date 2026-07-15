# Repository Guidelines

## 项目概览

Waveshare **ESP32-S3-RLCD-4.2** 开发板固件，基于 **ESP-IDF v6.0.2**（非 Arduino）。目标是打造一个桌面终端，集成：低功耗时钟+天气、语音交互(唤醒词+LLM对话)、环境监测记录(温湿度+SD打点)、Claude Code 用量额度显示，并逐步扩展为板载运行时、板载长期记忆的个人智能体。P0-P4 均已完成并真机验证；P5 起进入语音与智能体阶段。全项目注释与文档统一用中文。

## 架构与数据流

P0-P4 的实现先以页面和轮询服务完成真机闭环，存在页面直接读传感器/网络/SD、`ui_manager` 直接轮询按键、`app_main` 直接编排全部启动步骤三项遗留。它们不得在新功能中继续扩散；R0/P5 起目标架构如下：

```
main/app_main.c   只启动 app_runtime（≤100行硬限）
      ↓
app_runtime       启动顺序、注册、降级策略
      ├── input_manager     GPIO 消抖、长短按、语义输入事件
      ├── sys_state         状态快照、事件、命令
      ├── ui_manager/ui_pages  读取快照、渲染、提交命令
      ├── voice_assistant   一轮语音编排、取消
      │     └── agent_runtime  Pi 风格消息/工具循环、会话、记忆
      └── 平台与能力 Module
            storage_sd / cloud_transport / scheduler / 网络、传感器、音频、显示
                  ↓
             board_rlcd42   引脚定义 + 总线init（最底层，不依赖任何组件）
```

调用方只学习小 Interface，复杂度收进深 Module。`sys_state` 是跨 Module 的状态 seam；`llm_provider` 有 SiliconFlow、DeepSeek 两个生产 Adapter；`memory_engine` 只有板载实现，不为假想的远程记忆建立 provider 层。

当前 `app_main()` 的启动顺序仍是：芯片信息 → board/display/RTC/SHTC3/battery → SD/time/env logger（可选）→ UI 注册/启动 → 网络、额度、天气、SNTP。R0 把这段编排移入 `app_runtime`；`app_main()` 最终只保留芯片诊断与 `app_runtime_start()`。

**桥接服务**（`bridge/server.py`，跑在Mac本地，非ESP32固件的一部分）：ESP32通过mDNS解析Mac主机名+局域网HTTP轮询它，代理转发Claude订阅用量(`omp usage`)。天气(`weather_client`)不走桥接，设备经`cloud_transport`组件直连QWeather(HTTPS+TLS证书校验+ROM miniz解gzip)。详见 `bridge/README.md` 与 `CLAUDE.md` "P3 天气架构"一节。

## 关键目录

| 目录 | 用途 |
|---|---|
| `firmware/main/` | `app_main.c`(芯片诊断 + `app_runtime` 启动) + `secrets.h`(gitignore，本地凭证：WiFi/桥接服务mDNS主机名，模板见 `secrets.h.example`) |
| `firmware/components/board_rlcd42/` | 唯一的引脚定义与总线init(I2C/SPI/按键)，暴露handle与I2C扫描 |
| `firmware/components/display_st7305/` | ST7305反射屏驱动：1bpp帧缓冲、画点/矩形、ASCII字体(仅`0x20~0x5A`)、中文16×16字库(`font_cn16.c`，自动生成) |
| `firmware/components/ui_manager/` | 页面注册、导航、tick调度；`include/ui_page.h` 定义页面 Interface 契约；现有按键判定待迁入 `input_manager` |
| `firmware/components/ui_pages/` | `pages/page_home.c`(时钟/日期/室内温湿度)、`pages/page_env_monitor.c`(温湿度趋势)、`pages/page_quota.c`(Claude用量)、`pages/page_weather.c`(天气实况)、`pages/page_diag.c`(硬件自检，未注册进翻页，仅作**新页面参照范例**) |
| `firmware/components/sensor_shtc3/` | SHTC3温湿度读取，内部互斥锁防多任务并发访问 |
| `firmware/components/rtc_pcf85063/` | RTC读写(BCD转换)、掉电检测 |
| `firmware/components/net_manager/` | WiFi STA连接、断线自动重连 |
| `firmware/components/time_service/` | 启动从RTC恢复时间，联网后SNTP校时并写回RTC |
| `firmware/components/storage_sd/` | SD卡挂载(SDMMC 1-bit + FATFS) |
| `firmware/components/battery/` | 电池电压ADC读取 |
| `firmware/components/env_logger/` | 后台任务60s采样温湿度，环形缓冲(1小时窗口)+CSV落盘 |
| `firmware/components/claude_quota_client/` | mDNS解析桥接服务+HTTP轮询`/api/quota/compact`，60s周期 |
| `firmware/components/weather_client/` | 设备直连QWeather `/v7/weather/now`(HTTPS+cJSON解析)，经`cloud_transport`走TLS+gzip解压，15分钟周期 |
| `firmware/components/app_runtime/` | 启动顺序、页面/功能注册、可选能力降级 |
| `firmware/components/sys_state/` | 线程安全状态快照、事件、命令；UI/工具的唯一共享状态入口 |
| `firmware/components/input_manager/` | KEY/BOOT 消抖、长短按与语义输入事件 |
| `firmware/components/cloud_transport/` | HTTPS `get`(TLS+gzip自动解压)+流式`post_stream`(SSE/PCM逐块回调+取消)，weather_client/llm_provider/asr_client/tts_client共用 |
| `firmware/components/llm_provider/` | SiliconFlow/DeepSeek统一chat接口，SSE解析+tool_calls归一化+重试 |
| `firmware/components/asr_client/` | SiliconFlow语音转写(multipart上传SD卡WAV) |
| `firmware/components/tts_client/` | SiliconFlow语音合成(PCM流式回调) |
| `firmware/components/memory_engine/` *(P6)* | SD 上的会话、画像、长期记忆日志、索引、召回与维护 |
| `tools/` | `gen_font.py`(中文字库生成器) + `charset.txt`(手工补充字符集，仅运行时才出现的文案用) |
| `docs/` | `coding-standards.md`(强制开发规范)、`hardware-pinmap.md`(引脚权威表)、`vendor-provenance.md`(摘取代码来源登记) |
| `bridge/` | Mac本地HTTP桥接服务(`server.py`)：只代理Claude用量(P4)。天气(P3)已改为设备直连，不再经bridge |

## 开发命令

每个新终端必须先激活环境（`idf.py` 才在 PATH 里）：

```bash
conda activate esp-idf-tools
source ~/esp/esp-idf/export.sh
```

构建/烧录/监视（串口名形如 `/dev/cu.usbmodem*`，数字随USB口变化，用 `ls /dev/cu.usbmodem*` 探测；见Gotchas，USB会话失效时设备节点会换号）：

```bash
cd firmware
idf.py set-target esp32s3   # 首次配置
idf.py build
idf.py -p $(ls /dev/cu.usbmodem*) flash
idf.py monitor               # 交互式；脚本化场景见下方 Gotchas
```

启动Mac侧桥接服务（Claude用量页面需要它在线才有数据，否则降级显示"不可达"；天气页面不依赖bridge，设备直连QWeather）：

```bash
python3 bridge/server.py [--port 8787] [--refresh 60]
```

改中文字库（改了源码字符串或 `tools/charset.txt` 之后必须重跑，否则屏幕显示空心框）：

```bash
python3 tools/gen_font.py   # 扫描 firmware/ 中文字面量 + charset.txt，重写 font_cn16.c
```

无自动化测试套件、无 CI、无 lint/format 配置——当前阶段以**真机烧录验证**为准（见提交检查清单）。

## 代码约定与常见模式

规范权威来源：[`docs/coding-standards.md`](docs/coding-standards.md)（强制约定，非建议）。要点：

- **组件目录结构固定**：`components/<name>/{CMakeLists.txt, include/<name>.h, <name>.c}`。`include/<name>.h` 是唯一对外接口；内部辅助函数一律 `static`，不进头文件。
- **公开符号必须加组件前缀**（如 `board_*` / `st7305_*` / `rtc_pcf85063_*` / `ui_*`）。短前缀有跟 ESP-IDF 内部符号撞名的风险（曾发生 `rtc_init` 冲突，已改全前缀）。
- **文件/函数体积红线**：单 `.c` 文件 400 行；单函数 60 行；`app_main.c` 100 行；函数参数 5 个（超了改传结构体）。硬性红线，超了立即拆分——多个组件曾靠抽 `static` 辅助函数（如`log_if_failed()`）压回红线内。
- **错误处理**：驱动层/服务层**禁止 `ESP_ERROR_CHECK`**（会 `abort()` 重启整机）—— 一律返回 `esp_err_t`，可恢复错误配 `ESP_LOGW` 由上层重试/降级；只有 `app_main` 里板级初始化失败才允许 `ESP_ERROR_CHECK`。函数返回 `esp_err_t` 必须检查，不吞错误码。
- **页面 Interface 契约**（`components/ui_manager/include/ui_page.h`）：
  ```c
  typedef struct {
      const char *name;
      void (*on_enter)(void);              /* 可为NULL */
      void (*on_exit)(void);               /* 可为NULL */
      void (*on_tick)(void);               /* 可为NULL，周期由ui_manager控制 */
      bool (*on_key)(ui_key_event_t evt);  /* 返回true=已消费 */
  } ui_page_t;
  ```
  新增页面 = 照抄 `firmware/components/ui_pages/pages/page_diag.c` 结构（static 状态 + static draw() + on_enter/on_exit/on_tick/on_key + 文件末尾 `static const ui_page_t s_page` + getter），在 `ui_pages.h` 暴露 getter，由 `app_runtime` 注册。页面只读取 `sys_state` 快照、渲染和提交命令；不自己发 HTTP、不读传感器/电池/网络/SD。P0-P4 旧页面的直接读取仅可在迁移时删除，不得仿照扩展。
- **GPIO / 总线**：所有引脚宏只准定义在 `board_rlcd42.h`；总线init只在 `board_init()` 做一次，其他组件用 `board_i2c_bus()` 拿 handle。新增外设先更新 `docs/hardware-pinmap.md` 再写代码。
- **内存**：大缓冲(帧缓冲/音频/模型)显式指定 `MALLOC_CAP_SPIRAM` 或 `MALLOC_CAP_DMA`（DMA目标缓冲必须DMA-capable），不靠默认分配器。
- **FreeRTOS 任务**：栈大小/优先级集中登记在 `docs/coding-standards.md` 第7节表格，新增任务先登记避免优先级打架。事件回调中不得 `vTaskDelay`、网络访问、文件写入或等待锁；通过事件或定时器做重试。**多个任务并发调用同一I2C设备的多步命令时序(唤醒→测量→读取)必须加互斥锁**（`sensor_shtc3`踩过：`env_logger_task`与`ui_task`并发读导致时序错乱），不能假设I2C驱动单次事务线程安全等于设备协议时序安全。
- **按键**：`input_manager` 是 KEY/BOOT 的唯一 GPIO 所有者。KEY 短按 NEXT、长按 VOICE；BOOT 短按 ACTION/确认、长按 BACK/CANCEL；PWR 是硬件电源开关，不能分配固件功能。
- **云与记忆**：SiliconFlow/DeepSeek 的模型名、base URL、超时和凭据引用配置化。会话、Skill、任务和长期记忆都经 `storage_sd` 与板载 `memory_engine` 管理；云模型只可提议记忆，最终写入、检索、删除和索引必须在设备完成。
- **注释**：写"为什么"不写"是什么"；硬件魔数/寄存器序列(如ST7305交错位序)必须注释解释来源；全项目中文注释。
- **Vendor代码**：从官方demo摘任何代码片段必须同步在 `docs/vendor-provenance.md` 登记来源路径+commit，摘来的寄存器序列/魔数不准"顺手优化"。
- **密钥**：WiFi密码、和风天气API Key、SiliconFlow/DeepSeek API Key 等一律不进仓库，开发期放 `firmware/main/secrets.h`(已gitignore)，模板见同目录 `secrets.h.example`。
- **字体渲染陷阱**：ASCII点阵字体只收录 `0x20~0x5A`(空格到大写Z)，超范围字符(如`~`、小写字母)画成空心框而非报错，新增展示字符串前要检查。

## 重要文件

- `firmware/main/app_main.c` — 唯一入口，芯片诊断和 `app_runtime` 启动（不下沉业务逻辑到这里）
- `firmware/main/secrets.h.example` — 本地凭证模板（复制为 `secrets.h`）
- `firmware/CMakeLists.txt` — 顶层工程定义，`project(esp32_rlcd42_terminal)`
- `firmware/sdkconfig.defaults` — Kconfig覆盖项：16MB Flash(QIO@80MHz)、8MB Octal PSRAM、CPU 240MHz、自定义分区表、FATFS长文件名
- `firmware/partitions.csv` — 自定义分区表：`factory` app区 4MB、`model` 语音模型区 8MB(P5用)、`storage` 数据区 ~4MB；改动需全片擦除
- `firmware/components/ui_manager/include/ui_page.h` — 页面接口契约，新页面的唯一依据
- `firmware/components/ui_pages/pages/page_diag.c` — 新增页面的结构范例
- `firmware/components/board_rlcd42/include/board_rlcd42.h` — 全项目唯一引脚宏来源
- `tools/gen_font.py` / `tools/charset.txt` — 中文字库生成（输出 `firmware/components/display_st7305/font_cn16.c`，勿手工编辑）
- `bridge/server.py` — Mac本地桥接服务，只代理Claude用量(`omp usage`)
- `docs/coding-standards.md` — 强制开发规范（改代码前必读）
- `docs/voice-agent-design.md` — P5-P8 的板载智能体、记忆、联网工具与验收路线权威设计
- `docs/r0-ui-v2-spec.md` — P5 前置架构治理与现有四页 UI V2 的实施/验收规格
- `docs/hardware-pinmap.md` — 引脚/总线权威表
- `docs/vendor-provenance.md` — 摘取代码来源记录

## 运行时/工具链要求

- **框架**：ESP-IDF v6.0.2（装在 `~/esp/esp-idf`），非 Arduino。
- **工具链**：`~/.espressif`；cmake/ninja 等前置依赖装在 conda 环境 `esp-idf-tools`（不用 base，避免污染）——每个新终端必须 `conda activate esp-idf-tools && source ~/esp/esp-idf/export.sh`。
- **目标芯片**：`esp32s3`（ESP32-S3-WROOM-1-N16R8，双核240MHz，16MB Flash，8MB Octal PSRAM）。
- **依赖管理**：`claude_quota_client`(`espressif/mdns`+`espressif/cjson`)、`weather_client`(`espressif/cjson`)各自 `idf_component.yml` 声明，通过ESP-IDF Component Manager自动拉取到 `firmware/managed_components/`。board级特有代码(如ST7305初始化寄存器序列)按需摘取到自建component并登记 `docs/vendor-provenance.md`。**不整仓引入**官方 demo 仓库。
- **字库生成脚本**依赖 Pillow(`PIL`)，用系统字体 `/System/Library/Fonts/STHeiti Medium.ttc`(macOS专用路径)。
- **桥接服务**（`bridge/server.py`）纯Python标准库，零pip依赖；只代理Claude用量，依赖 `omp` CLI在PATH。天气(`weather_client`)设备直连QWeather，凭据配在 `firmware/main/secrets.h`。
- **macOS 没有 `timeout` 命令**：脚本化抓串口日志改用 ESP-IDF venv 的 pyserial（`~/.espressif/python_env/idf6.0_py3.11_env/bin/python`）。
- 改了 `sdkconfig.defaults` 必须 `rm -f firmware/sdkconfig` 再 build，否则已生成的 `sdkconfig` 会覆盖默认值、改动静默失效。

## 测试与QA

**无自动化测试套件**（无 unit test、无 CI、无 lint/format 配置）——验证方式是**真机烧录 + 实测**，按 `CLAUDE.md` 中的分阶段路线图逐阶段验证。Claude用量额外靠桥接服务访问日志确认ESP32真实IP发起过成功请求；天气额外靠设备日志确认 `esp-x509-crt-bundle: Certificate validated`(TLS证书链校验通过)，不只是"编译通过"。

提交前检查清单（`docs/coding-standards.md` 第12节）：
- [ ] 没有裸 GPIO 号散落在 `board_rlcd42` 之外
- [ ] 驱动/服务层没有 `ESP_ERROR_CHECK`
- [ ] 没有文件超400行 / 函数超60行 / `app_main.c` 超100行
- [ ] 没有横向或向上的依赖
- [ ] 新页面不直接读取设备/网络/SD；新增按键逻辑未绕过 `input_manager`
- [ ] 新任务已在 `docs/coding-standards.md` 第7节表格登记
- [ ] 新摘取的 vendor 代码已在 `docs/vendor-provenance.md` 登记
- [ ] 密钥没进仓库
- [ ] **真机验证过**（不是只编译通过）
