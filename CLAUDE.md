# CLAUDE.md

> 最后更新：2026-07-18
> 写代码前先读 [`docs/coding-standards.md`](docs/coding-standards.md)。架构与验收状态以 [`docs/voice-agent-design.md`](docs/voice-agent-design.md) 为准；引脚与供应商代码来源分别以 [`docs/hardware-pinmap.md`](docs/hardware-pinmap.md)、[`docs/vendor-provenance.md`](docs/vendor-provenance.md) 为准。

## 当前基线

Waveshare ESP32-S3-RLCD-4.2 的 ESP-IDF v6.0.2 固件，目标是设备端个人 Jarvis 智能体。设备是身份、状态、会话、记忆、任务和权限的唯一权威；云端只提供 ASR、LLM、TTS 与受限公开网络检索。

- P5.2 单轮语音闭环核心已真机验证：MultiNet 命令词“贾维斯”→ 录音 → ASR → agent → 流式 TTS。
- P6.0 agent、P6.1 Web / Skills、P6.2 记忆 M1、P6.3 提醒 M1 已实现；SD、Jina 和提醒的完整真机回归仍未完成，不能在发布说明中称为已完成。
- 当前工作重心是四页 UI 的纯视觉重构：保留页面注册顺序、按键行为、`sys_state` 数据路径和所有降级逻辑。已移除 `page_quota`，剩余翻页页面为 jarvis / home / weather / env_monitor。

## 架构

```text
main/app_main.c
      |
app_runtime                         启动顺序、注册、降级
  +-- input_manager                 KEY / BOOT 的唯一 GPIO 所有者
  +-- sys_state                     跨模块状态、命令、提醒邮箱
  +-- ui_manager / ui_pages         读取快照、渲染、提交命令
  +-- voice_assistant               单轮语音、取消、提醒播报
  |     +-- agent_runtime           消息、模型、固定工具循环、会话
  |           +-- skill_manager / memory_engine
  +-- scheduler                     一次与周期提醒
  +-- 平台与能力模块                网络、SD、音频、显示、传感器、云传输
        |
    board_rlcd42                    唯一引脚定义与总线初始化所有者
```

- 跨模块异步状态只能经 `sys_state`；不得以可写全局变量、页面直读或临时 include 绕过。
- 页面只读一次 `sys_state` 快照并渲染/提交命令；不得发 HTTP、读网络、传感器、电池、音频或 SD。`page_diag.c` 是未注册的旧式诊断页，不是范例。
- `input_manager` 是物理按键唯一所有者。KEY 仅短按翻页；BOOT 仅用于真实确认或语音活动时的长按取消；PWR 不分配固件行为。
- `app_main.c` 只做芯片诊断与 `app_runtime_start()`；业务初始化、注册和降级归 `app_runtime`。

## 关键组件

| 组件 | 职责 |
|---|---|
| `cloud_transport` | TLS 证书校验、GET、JSON、流式 POST、取消、总超时；云客户端共用。 |
| `llm_provider` | SiliconFlow / DeepSeek 的 SSE 与工具调用归一化。 |
| `asr_client` / `tts_client` | SiliconFlow ASR 与 PCM 流式 TTS；TTS 复用连接并在陈旧连接时重连一次。 |
| `agent_runtime` | 最多四轮串行模型/工具循环；工具 Schema 固定。 |
| `web_client` / `skill_manager` | Jina 来源受限检索与 SD Skill 的白名单/CRC32 校验。 |
| `memory_engine` | 板载 recall / record / forget / maintain；SD 不可用时独立降级。 |
| `scheduler` | 最多 16 条一次或周期提醒；通过 `sys_state` 邮箱投递。 |
| `storage_sd` | 受锁的追加、流式读、目录枚举和原子替换。 |

公开网页、Skill、模型输出和工具结果都不可信：不得借此新增工具、路径、权限、system prompt 或远程执行能力。

## 开发命令

每个新终端先激活 ESP-IDF 环境：

```bash
source "$(conda info --base)/etc/profile.d/conda.sh"
conda activate esp-idf-tools
source ~/esp/esp-idf/export.sh
cd firmware
```

```bash
idf.py build
PORT=/dev/cu.usbmodem1101  # 按实际设备替换
idf.py -p "$PORT" flash
idf.py -p "$PORT" monitor
```

- 第一次配置目标：`idf.py set-target esp32s3`。
- 修改 `sdkconfig.defaults` 后先执行 `rm -f sdkconfig`，再构建。
- 修改固件中的中文字符串或 `tools/charset.txt` 后执行 `python3 tools/gen_font.py`；禁止手改 `font_cn16.c`。
- 无自动化测试、CI 或格式化配置。行为变更必须烧录，并观察受影响路径；语音相关变更至少确认日志出现 `voice_assistant: 就绪`。

## 不可违反的实现约束

- 组件结构固定为 `components/<name>/{CMakeLists.txt,include/<name>.h,<name>.c}`；外部只 include 公开头文件，内部辅助函数一律 `static`，公开符号带组件前缀。
- `.c` 文件最多 400 行、函数最多 60 行、`app_main.c` 最多 100 行、函数最多 5 个参数；超限立即按职责拆分。
- 驱动和服务层禁止 `ESP_ERROR_CHECK`；返回的 `esp_err_t` 必须检查，可恢复问题由上层重试或降级。
- 所有 GPIO 宏只在 `board_rlcd42.h`；总线只由 `board_init()` 初始化。新增外设先更新引脚文档。
- 音频、帧缓冲和其他大缓冲显式使用 `MALLOC_CAP_SPIRAM` 或 `MALLOC_CAP_DMA`；DMA 目标必须 DMA-capable。
- 新 FreeRTOS 任务先登记 `coding-standards.md` 的任务表。事件回调不阻塞、不访问网络、不写文件、不等待锁；同一 I2C 设备的多步协议必须由互斥锁保护。
- 密钥只放 `firmware/main/secrets.h`，不得提交。`JINA_API_KEY` 缺失只禁用 Web 工具；SD 缺失必须让短会话和普通语音继续运行。
- UI 使用 400×300、1bpp ST7305；ASCII 字库仅覆盖 `0x20..0x5A`，超范围字符会显示空心框。正常状态保持安静，只为异常、确认和可恢复操作显示短文案。
- 摘取官方 demo 代码必须同步登记来源路径与 commit；寄存器序列和硬件魔数不可随意改写。

## 验收与交付

构建通过不是验收。涉及行为的变更须烧录实测；天气还须确认 TLS 证书链校验日志。提交前按 `coding-standards.md` 清单检查依赖方向、任务登记、大小红线、密钥、Vendor 来源和真机证据。

仍待完成的真机验收集中记录在 `docs/voice-agent-design.md` 第 7 节：多句 TTS/弱网回归、连续 20 轮语音、SD 记忆、Jina Web/Skill、提醒的创建/取消/重启/拔卡降级。
