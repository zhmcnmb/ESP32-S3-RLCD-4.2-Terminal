# Repository Guidelines

## 项目概览

Waveshare **ESP32-S3-RLCD-4.2** 开发板固件，基于 **ESP-IDF v6.0.2**（非 Arduino）。目标是打造一个极简桌面终端，并逐步升级为拥有专属头像、语音、工具、会话和板载长期记忆的个人 Jarvis 智能体。P0-P4、R0、P5.0、P5.1 已完成；当前进入 P5.2 单轮语音闭环与 Jarvis 主页面阶段。全项目注释与文档统一用中文。

## 架构与数据流

R0 已完成页面状态、输入和启动编排迁移；当前实际架构如下，P5.2 继续沿用，不得为 Jarvis 页面或语音闭环开直读后门：

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

当前 `app_main()` 已只保留芯片诊断、`app_runtime_start()` 与 P5.1 临时自检入口。P5.2 必须删除 `p51_selftest`，改由正式 `voice_assistant` 接管 wakeword/ASR/LLM/TTS 状态。

**Mac bridge 是 Claude 额度页的可选增强**：该页先保留，因为数据源依赖本机 `omp` 登录态；Mac 不在线时只允许额度页降级，Jarvis、语音、天气、环境、agent、记忆和任务不得受影响。

## 关键目录

| 目录 | 用途 |
|---|---|
| `firmware/main/` | `app_main.c`(芯片诊断 + `app_runtime` 启动) + `secrets.h`(gitignore，本地凭证：WiFi/桥接服务mDNS主机名/云API Key，模板见 `secrets.h.example`) |
| `firmware/components/board_rlcd42/` | 唯一的引脚定义与总线init(I2C/SPI/按键)，暴露handle与I2C扫描 |
| `firmware/components/display_st7305/` | ST7305反射屏驱动：1bpp帧缓冲、画点/矩形、ASCII字体(仅`0x20~0x5A`)、中文16×16字库(`font_cn16.c`，自动生成) |
| `firmware/components/ui_manager/` | 页面注册、导航、tick调度；只消费 `input_manager` 语义事件，不读 GPIO |
| `firmware/components/ui_pages/` | 当前四个只读数据页；P5.2 新增默认 `page_jarvis`，以专属头像显示智能体状态，具体样式不在规则中固定。`page_diag.c` 是旧式直读诊断页，不得作为新页面模板 |
| `firmware/components/sensor_shtc3/` | SHTC3温湿度读取，内部互斥锁防多任务并发访问 |
| `firmware/components/rtc_pcf85063/` | RTC读写(BCD转换)、掉电检测 |
| `firmware/components/net_manager/` | WiFi STA连接、断线自动重连 |
| `firmware/components/time_service/` | 启动从RTC恢复时间，联网后SNTP校时并写回RTC |
| `firmware/components/storage_sd/` | SD卡挂载(SDMMC 1-bit + FATFS)、受锁的追加/流式读/目录枚举/原子替换 |
| `firmware/components/battery/` | 电池电压ADC读取 |
| `firmware/components/env_logger/` | 后台任务60s采样温湿度，环形缓冲(1小时窗口)+CSV落盘 |
| `firmware/components/claude_quota_client/` | 保留的可选 Claude 额度页数据源；Mac 离线时独立降级 |
| `firmware/components/weather_client/` | 设备直连QWeather `/v7/weather/now`(HTTPS+cJSON解析)，经`cloud_transport`走TLS+gzip解压，15分钟周期 |
| `firmware/components/app_runtime/` | 启动顺序、页面/功能注册、可选能力降级 |
| `firmware/components/sys_state/` | 线程安全状态快照、事件、命令；UI/工具的唯一共享状态入口 |
| `firmware/components/input_manager/` | KEY/BOOT 物理输入所有者；当前 KEY 短按翻页，未接入的长按说话占位将在 P5.2 删除 |
| `firmware/components/cloud_transport/` | HTTPS `get`(TLS+gzip自动解压)、流式`post_stream`(SSE/PCM逐块回调+取消)和有界`post_json`，云客户端共用 |
| `firmware/components/llm_provider/` | SiliconFlow/DeepSeek统一chat接口，SSE解析+tool_calls归一化+重试 |
| `firmware/components/asr_client/` | SiliconFlow语音转写(multipart上传SD卡WAV) |
| `firmware/components/tts_client/` | SiliconFlow语音合成(PCM流式回调) |
| `firmware/components/web_client/` | Jina Search/Reader 的受限 Adapter：最多5个来源、仅可抽取最近搜索来源、16KB响应与8KB正文上限 |
| `firmware/components/skill_manager/` | SD声明式Skill索引、静态工具白名单校验、正文CRC32复核和按需加载；SD缺失时不影响普通agent |
| `firmware/components/memory_engine/` *(P6.2 M1基线已构建，待SD真机验收)* | 板载长期记忆的 recall/record/forget/maintain Interface；内部处理带CRC的JSONL、tombstone、画像双槽快照、TTL、互斥状态访问与有界本地召回，SD缺失时禁用记忆而不影响短会话 |
| `tools/` | `gen_font.py`(中文字库生成器) + `charset.txt`(手工补充字符集，仅运行时才出现的文案用) |
| `docs/` | `coding-standards.md`(强制开发规范)、`hardware-pinmap.md`(引脚权威表)、`vendor-provenance.md`(摘取代码来源登记) |
| `bridge/` | 开发期可选工具，只代理Claude用量；正式产品不依赖 |

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

使用 Claude 用量页时启动 Mac bridge；不启动时其余 Jarvis 功能必须正常：

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
  新增页面参考已迁移的 `page_home.c` 结构，实现 `ui_page_t`，在 `ui_pages.h` 暴露 getter，由 `app_runtime` 注册。页面只读取一次 `sys_state` 快照、渲染和提交真实命令；不自己发 HTTP、不读传感器/电池/网络/SD。`page_diag.c` 仍直接访问硬件，只能用于诊断，禁止照抄其依赖方式。
- **GPIO / 总线**：所有引脚宏只准定义在 `board_rlcd42.h`；总线init只在 `board_init()` 做一次，其他组件用 `board_i2c_bus()` 拿 handle。新增外设先更新 `docs/hardware-pinmap.md` 再写代码。
- **内存**：大缓冲(帧缓冲/音频/模型)显式指定 `MALLOC_CAP_SPIRAM` 或 `MALLOC_CAP_DMA`（DMA目标缓冲必须DMA-capable），不靠默认分配器。
- **FreeRTOS 任务**：栈大小/优先级集中登记在 `docs/coding-standards.md` 第7节表格，新增任务先登记避免优先级打架。事件回调中不得 `vTaskDelay`、网络访问、文件写入或等待锁；通过事件或定时器做重试。**多个任务并发调用同一I2C设备的多步命令时序(唤醒→测量→读取)必须加互斥锁**（`sensor_shtc3`踩过：`env_logger_task`与`ui_task`并发读导致时序错乱），不能假设I2C驱动单次事务线程安全等于设备协议时序安全。
- **按键**：`input_manager` 是 KEY/BOOT 的唯一 GPIO 所有者。KEY 当前只用短按 NEXT；未接入功能不显示、不做占位。BOOT 仅在真实页面动作或 agent 确认态使用，长按只在 agent 活动时解释为 CANCEL。PWR 是硬件电源开关，不能分配固件功能。
- **UI 产品方向**：极简现代科幻；正常状态不解释，异常才显示短文案；普通页面不常驻操作教程。默认 `page_jarvis` 使用原创机械面甲风专属头像，可借鉴变形金刚式结构感但不得照搬具体角色，最终样式由后续设计评审决定。
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
- `firmware/components/ui_pages/pages/page_home.c` — 只读快照页面的结构范例
- `firmware/components/board_rlcd42/include/board_rlcd42.h` — 全项目唯一引脚宏来源
- `tools/gen_font.py` / `tools/charset.txt` — 中文字库生成（输出 `firmware/components/display_st7305/font_cn16.c`，勿手工编辑）
- `bridge/server.py` — Claude 用量页的可选代理；其余功能不依赖
- `docs/coding-standards.md` — 强制开发规范（改代码前必读）
- `docs/voice-agent-design.md` — P5-P8 的板载智能体、记忆、联网工具与验收路线权威设计
- `docs/hardware-pinmap.md` — 引脚/总线权威表
- `docs/vendor-provenance.md` — 摘取代码来源记录

## 运行时/工具链要求

- **框架**：ESP-IDF v6.0.2（装在 `~/esp/esp-idf`），非 Arduino。
- **工具链**：`~/.espressif`；cmake/ninja 等前置依赖装在 conda 环境 `esp-idf-tools`（不用 base，避免污染）——每个新终端必须 `conda activate esp-idf-tools && source ~/esp/esp-idf/export.sh`。
- **目标芯片**：`esp32s3`（ESP32-S3-WROOM-1-N16R8，双核240MHz，16MB Flash，8MB Octal PSRAM）。
- **依赖管理**：`claude_quota_client`(`espressif/mdns`+`espressif/cjson`)、`weather_client`(`espressif/cjson`)各自 `idf_component.yml` 声明，通过ESP-IDF Component Manager自动拉取到 `firmware/managed_components/`。board级特有代码(如ST7305初始化寄存器序列)按需摘取到自建component并登记 `docs/vendor-provenance.md`。**不整仓引入**官方 demo 仓库。
- **字库生成脚本**依赖 Pillow(`PIL`)，用系统字体 `/System/Library/Fonts/STHeiti Medium.ttc`(macOS专用路径)。
- **桥接服务**（`bridge/server.py`）只服务 Claude 用量页；Mac 离线不得影响其余功能。天气(`weather_client`)设备直连QWeather，凭据配在 `firmware/main/secrets.h`。
- **macOS 没有 `timeout` 命令**：脚本化抓串口日志改用 ESP-IDF venv 的 pyserial（`~/.espressif/python_env/idf6.0_py3.11_env/bin/python`）。
- 改了 `sdkconfig.defaults` 必须 `rm -f firmware/sdkconfig` 再 build，否则已生成的 `sdkconfig` 会覆盖默认值、改动静默失效。

## 测试与QA

**无自动化测试套件**（无 unit test、无 CI、无 lint/format 配置）——验证方式是**真机烧录 + 实测**，按 `CLAUDE.md` 中的分阶段路线图逐阶段验证。正式功能必须在不启动 Mac bridge 时完成验收；天气额外靠设备日志确认 `esp-x509-crt-bundle: Certificate validated`(TLS证书链校验通过)，不只是"编译通过"。

提交前检查清单（`docs/coding-standards.md` 第12节）：
- [ ] 没有裸 GPIO 号散落在 `board_rlcd42` 之外
- [ ] 驱动/服务层没有 `ESP_ERROR_CHECK`
- [ ] 没有文件超400行 / 函数超60行 / `app_main.c` 超100行
- [ ] 没有横向或向上的依赖
- [ ] 新页面不直接读取设备/网络/SD；新增按键逻辑未绕过 `input_manager`
- [ ] 正常状态无冗余成功说明，未接入功能无入口或占位提示
- [ ] 新任务已在 `docs/coding-standards.md` 第7节表格登记
- [ ] 新摘取的 vendor 代码已在 `docs/vendor-provenance.md` 登记
- [ ] 密钥没进仓库
- [ ] **真机验证过**（不是只编译通过）
