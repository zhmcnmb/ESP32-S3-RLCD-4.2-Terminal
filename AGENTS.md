# Repository Guidelines

> 最后更新：2026-07-18。全项目注释与文档使用中文。

## 项目与当前状态

这是 Waveshare **ESP32-S3-RLCD-4.2** 的 ESP-IDF v6.0.2 固件，不是 Arduino 工程。目标是设备端个人 Jarvis 智能体：设备管理状态、会话、记忆、任务与权限，云端仅提供 ASR、LLM、TTS、公开只读用量查询与受限 Web 检索。

- P5.2 单轮语音闭环核心已真机验证。
- P6.0 agent、P6.1 Web / Skills、P6.2 记忆 M1、P6.3 提醒 M1 已实现；免 Key Web 检索主链路（搜索/抽取）已验收，SD、Skill CRC、提醒等完整真机验收仍未完成，不能表述为已完成。
- 当前 UI 工作只改视觉；不得改变已定按键语义(KEY短/长=NEXT/PREV，BOOT短/长=PTT/CANCEL)、`sys_state` 数据路径或任何降级逻辑。
- 六个翻页页面为 Jarvis / 时钟 / 天气 / 环境趋势 / REECHI用量 / WiFi；调试页已从翻页移除。

## 权威文档

| 文档 | 用途 |
|---|---|
| `docs/coding-standards.md` | 强制编码规范、任务表和提交清单；改代码前必读。 |
| `docs/voice-agent-design.md` | 当前架构、能力边界、数据格式、阶段状态与未完成验收。 |
| `docs/hardware-pinmap.md` | 引脚与总线的唯一权威来源。 |
| `docs/vendor-provenance.md` | 外部代码和硬件序列的来源登记。 |
| `firmware/main/secrets.h.example` | 本地凭据模板；实际 `secrets.h` 不提交。 |

## 架构与依赖方向

```text
main/app_main.c
      |
app_runtime                         启动、注册、降级
  +-- input_manager                 KEY / BOOT 的唯一 GPIO 所有者
  +-- sys_state                     快照、事件、命令、提醒邮箱
  +-- ui_manager / ui_pages         读取快照、渲染、提交命令
  +-- voice_assistant               语音编排、取消、提醒播报
  |     +-- agent_runtime           消息、模型、固定工具循环
  |           +-- skill_manager / memory_engine
  +-- scheduler                     一次和周期提醒
  +-- wifi_setup                    按键配网状态机(扫描/列表/密码)，NVS凭据优先
  +-- 平台与能力模块                SD、网络、音频、显示、传感器、云传输、text_console(串口文本注入,验收入口)
        |
    board_rlcd42                    引脚定义与总线初始化
```

- `app_main.c` 只做芯片诊断和 `app_runtime_start()`；初始化顺序、页面/功能注册和降级归 `app_runtime`。
- `sys_state` 是跨 Module 的唯一状态 seam。禁止可写全局变量、横向服务调用和临时 include 后门。
- 页面只读取 `sys_state` 快照、渲染和提交命令；不得读取网络、传感器、电池、音频或 SD，也不得自行发 HTTP。新增页面实现 `ui_page_t`，在 `ui_pages.h` 暴露 getter，由 `app_runtime` 注册。
- 新页面实现 `ui_page_t`，在 `ui_pages.h` 暴露 getter，由 `app_runtime` 注册。
- `board_rlcd42` 是最底层，不依赖 UI、语音、agent 或业务 Module。

## 关键能力边界

- 语音输入唯一入口是 BOOT 短按 PTT（2026-08 起开放式唤醒词/MultiNet 已下线，无常驻监听）；录音管线（AFE/VAD、`wakeword` 任务）仅在对话轮期间运行，成功/失败/取消都即停。`voice_assistant` 管理 `IDLE → LISTENING → TRANSCRIBING → THINKING/EXECUTING → SPEAKING → IDLE`，错误和取消必须回到 `IDLE`。
- `agent_runtime` 仅使用固定 JSON Schema 工具，最多四次串行模型/工具往返。模型、网页、Skill 和工具结果均不可信，不能新增工具、路径、权限、system prompt 或远程执行。
- `web_extract` 只允许抓取同轮 `web_search` 返回的 URL；Web 工具完全免 Key（Bing 搜索 + 白名单 URL 直连抓取正文）。
- `memory_engine` 提供板载 recall / record / forget / maintain。SD 缺失时记忆不注册，短会话与普通语音继续运行。
- `scheduler` 通过 `sys_state` 的单槽提醒邮箱投递；无 SD 时仅 RAM 降级，重启后丢失但不报错。

## 组件与代码规则

- 组件固定为 `components/<name>/{CMakeLists.txt,include/<name>.h,<name>.c}`。外部只 include 公开头文件；内部辅助函数一律 `static`；公开符号必须带组件前缀。
- `.c` 文件最多 400 行、函数最多 60 行、`app_main.c` 最多 100 行、函数最多 5 个参数；超过即拆分，不留“以后处理”。
- 驱动和服务层禁止 `ESP_ERROR_CHECK`；一切 `esp_err_t` 必须检查。可恢复失败返回错误并由上层重试或降级；只有 `app_main` 的不可恢复板级启动路径可使用它。
- 所有 GPIO 宏只定义于 `board_rlcd42.h`；总线只在 `board_init()` 初始化一次。新增外设先更新引脚文档。
- 音频、帧缓冲和其他大缓冲显式使用 `MALLOC_CAP_SPIRAM` 或 `MALLOC_CAP_DMA`；DMA 目标必须 DMA-capable。
- 新 FreeRTOS 任务必须先登记 `coding-standards.md` 第 7 节的栈和优先级。事件回调不允许阻塞、延时、网络访问、文件写入或等待锁；同一 I2C 设备的多步时序必须受互斥锁保护。
- 云端模型名称、URL、超时和凭据须配置化。`llm_provider` 的生产 Adapter 仅为 SiliconFlow 与 DeepSeek；不为假想实现创建 provider 层。
- 从官方 demo 摘代码时必须同步登记来源路径与 commit；寄存器序列、硬件魔数不做“顺手优化”。

## 输入与 UI

- `input_manager` 是 KEY/BOOT 唯一 GPIO 所有者。KEY 短按 NEXT、长按 PREV；BOOT 短按空闲时 push-to-talk、长按 CANCEL；PWR 不可分配固件功能。
- UI 是 400×300、1bpp 的 ST7305 反射屏。正常状态安静，不重复说明成功状态或按键教程；异常、确认与可恢复动作才显示短文案。
- 六个翻页页面(jarvis/home/weather/env_monitor/codex_usage/wifi)的视觉改动只能调整排版和绘制，不能改功能。
- WiFi 页是唯一例外按键模态：BOOT 短按进入配网流程后，KEY/BOOT 全部翻译为配网命令(选择/确认/删除)，逐层 BACK 退出才恢复翻页；配网状态机归 `wifi_setup` 组件，页面只读快照投命令。
- WiFi 凭据配网成功后写 NVS，`net_init` 优先使用 NVS 凭据，`secrets.h` 仅作首次兜底；配网失败自动恢复旧凭据，不会留下断网状态。
- ASCII 字库覆盖 `0x20..0x7A`（含小写字母与常用符号）。改动固件中文字符串或 `tools/charset.txt` 后必须运行 `python3 tools/gen_font.py`；不要手改 `font_cn16.c`。

## 环境、构建与烧录

每个新终端都先执行：

```bash
source "$(conda info --base)/etc/profile.d/conda.sh"
conda activate esp-idf-tools
source ~/esp/esp-idf/export.sh
cd firmware
```

```bash
idf.py set-target esp32s3  # 首次配置时
idf.py build
PORT=/dev/cu.usbmodem1101  # 按实际设备替换
idf.py -p "$PORT" flash
idf.py -p "$PORT" monitor
```

- 改 `sdkconfig.defaults` 后必须执行 `rm -f sdkconfig`，再构建。
- 密钥只放 `firmware/main/secrets.h`，不得提交。
- 无单元测试、CI、lint 或 formatter。行为变更必须构建、烧录并实测受影响路径；语音改动至少确认 `voice_assistant: 就绪`。
- 天气还须在日志中确认 `esp-x509-crt-bundle: Certificate validated`。

## 提交前

按 `docs/coding-standards.md` 的检查清单逐项确认：依赖方向、GPIO、错误处理、文件/函数红线、页面/按键边界、任务登记、Vendor 来源、密钥和真机证据。未完成的多句 TTS/弱网、连续语音、SD 记忆、Skill CRC/拔卡、提醒回归见 `docs/voice-agent-design.md` 第 7 节；不得把未验收事项写成完成。
