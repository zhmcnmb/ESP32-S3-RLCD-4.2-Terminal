# 设备端个人智能体长期架构与开发路线

> 状态：方案设计，尚未实现
> 日期：2026-07-15
> 适用硬件：ESP32-S3-WROOM-1-N16R8、ES7210、ES8311、Micro SD
> 架构原则：设备掌握身份、状态、权限、会话和长期记忆；云端只提供模型推理、公开网络检索与可选重型工具。

## 1. 最终方案

新增语音智能体采用“**设备自治 + 云端推理**”：

| 项目 | 决策 |
|---|---|
| 唤醒和音频前端 | ESP32-S3 本地运行 ESP-SR AFE、WakeNet、VAD |
| ASR | 设备上传一句完整录音到 SiliconFlow |
| 智能体 | ESP-IDF/C 实现 Pi 风格 agent loop |
| LLM | `llm_provider` 统一接口，设备直连 SiliconFlow 或 DeepSeek API |
| 工具 | 设备本地静态白名单工具 |
| TTS | SiliconFlow 返回 16k PCM，设备流式播放 |
| 当前会话 | SD 卡 JSONL，断网可用 |
| 长期记忆 | 板载 `memory_engine`；数据、索引、召回、遗忘与维护全部在 SD 卡 |
| 扩展 | 固件 Tool、SD 声明式 Skill、远程 Capability adapter |
| Mac bridge | 不参与新增语音主链路，仅保留现有功能；后期可演进为可选 Companion |

完整闭环：

```text
本地唤醒 -> 本地录音/VAD -> 云 ASR -> 设备端 agent loop
        -> 本地工具 -> 云 LLM -> 云 TTS -> 本地扬声器
```

这条链路不要求 Mac 常驻。现有天气和 Claude 用量页面仍按当前实现依赖 Mac `bridge`，本方案不顺带重构它们。

## 2. Pi 的使用边界

个人智能体继续采用 Pi Agent Harness 的核心架构，不直接移植 Pi runtime：

- 消息上下文：`system/user/assistant/tool`。
- 模型流式事件。
- `tool_calls -> tool result -> 再次请求模型`。
- 上下文裁剪、取消和会话持久化。
- Tool、Skill、Prompt 与 Provider 分层扩展。

官方 `@earendil-works/pi-agent-core` 是 TypeScript/ESM npm 包，要求 Node.js >= 22.19.0，不能运行在 ESP-IDF/FreeRTOS 上。因此固件新增的是精简 `agent_runtime`，不是 Pi npm 依赖。

复用分为三级：

| 级别 | 内容 | 本项目做法 |
|---|---|---|
| A：直接复用数据/协议 | `SKILL.md`、frontmatter、REST JSON、工具 JSON Schema | 存入 SD 或固件资源 |
| B：借鉴后用 C 重写 | agent loop、Web 工具、记忆策略、Cron、HA/Telegram adapter | 独立 ESP-IDF 模块 |
| C：设备端不运行 | npm/TypeScript Extension、Node、Chrome、完整 MCP、脚本/子进程 | 云 API 或可选 Companion |

首版明确不实现：

- Pi CLI、TUI、RPC 和插件系统。
- 任意代码或 shell 执行。
- 动态安装工具。
- 并行工具、多智能体和会话分支树。
- 在 ESP32 上嵌入 Node.js。

Pi Package Gallery 是发现目录，不是安全审核目录。设备不得下载执行第三方 npm、TypeScript 或 Skill 附带脚本；只允许加载固定来源、校验通过的声明式 Skill，Skill 最终只能调用固件白名单工具。

### 2.1 能力分级

长期升级按能力层推进，每层都能独立运行和验收：

| 层级 | 能力 | 对外依赖 |
|---|---|---|
| L0 桌面终端 | 时钟、天气、环境、额度页面 | 现有 bridge |
| L1 语音终端 | 唤醒、ASR、普通问答、TTS | SiliconFlow；LLM 可用 DeepSeek |
| L2 设备智能体 | Pi agent loop、本地工具、SD 会话 | SiliconFlow 或 DeepSeek LLM API |
| L3 联网智能体 | Web Search、网页提取、SD Skills | 搜索/提取 API |
| L4 个人智能体 | 板载长期记忆、偏好、备忘、Cron | LLM API；记忆本身无外部服务依赖 |
| L5 主动助手 | Home Assistant、Telegram 单通道 | 对应外部系统 |
| L6 增强模式 | Browser、HTTP MCP、远程执行、多通道 | 可选 Companion |

L1-L4 是本开发板的主线；L5 按真实使用需求选择；L6 不作为设备端必达目标。

## 3. 系统架构

```text
main/app_main.c
      |
app_runtime                    启动顺序、功能注册、降级策略
      |
      +-- input_manager        KEY/BOOT 消抖、长短按、语义事件
      +-- sys_state            状态快照、命令和事件
      +-- ui_pages             只渲染快照、提交命令
      |
voice_assistant                语音状态机、turn_id、取消
      |
agent_runtime                  Pi 风格 message -> model -> tool 循环
      +-- agent_tool_registry  工具 schema、权限、超时、输出预算
      +-- agent_skill_loader   SD Skills 索引与渐进加载
      +-- agent_session        当前会话、上下文预算、裁剪
      +-- memory_engine        SD 日志、索引、召回、遗忘、维护
      +-- scheduler            持久任务、RTC/NTP、错过任务策略
      |
capability adapters
      +-- siliconflow_client   ASR、Chat SSE、TTS、可选 Embedding
      +-- deepseek_client      Chat SSE、Function Calling
      +-- web_provider         Search、Extract、引用
      +-- ha_provider          Home Assistant 白名单操作
      +-- channel_provider     设备 UI；后期 Telegram
      +-- companion_client     可选 Browser、MCP、远程执行
      |
platform modules
      +-- cloud_transport      TLS、流式 body、SSE、取消、重试
      +-- storage_sd           文件、原子写、离线队列
      +-- wakeword             AFE、WakeNet、VAD
      +-- audio_pipeline       ES7210/ES8311、I2S、录放缓冲
      |
board_rlcd42
```

### 3.1 分层要求

- `page_voice` 不直接访问网络、音频或 SD。
- 所有页面只读 `sys_state` 快照，通过命令/event 请求动作。
- `voice_assistant` 是语音编排模块，拥有一轮对话的状态。
- `agent_runtime` 不知道 UI 和音频，只处理消息与工具循环。
- `agent_runtime` 只依赖稳定 provider interface，不包含供应商 URL。
- 工具读取状态快照或调用注册 adapter，不横向访问其他模块实现。
- 大音频缓冲使用 PSRAM，I2S DMA 缓冲使用 DMA-capable 内存。

仓库规范已经多处引用 `sys_state`，但当前尚无对应组件。P5 开始前只补一个最小实现：线程安全状态快照 + ESP-IDF event loop，不在其中堆业务逻辑。

P0-P4 现有页面直接读取设备数据、`ui_manager` 直接轮询按键、`app_main` 直接编排启动都属于迁移遗留。R0 逐项替换它们，但不为整理架构重写已真机验证的功能；从本方案生效起，新增页面只能读取 `sys_state`，新增按键逻辑只能进入 `input_manager`，新增启动/注册逻辑只能进入 `app_runtime`。不得同时维护旧直连路径和新状态路径。

### 3.2 深模块与 seams

长期架构优先形成少量深模块：调用者学习很小的 interface，复杂实现集中在模块内部。

| 模块 | 对外 interface | 隐藏的实现复杂度 |
|---|---|---|
| `sys_state` | read snapshot、apply event、post command | 锁、事件循环、状态版本 |
| `input_manager` | 发布语义输入事件 | GPIO、消抖、长短按、上下文映射 |
| `storage_sd` | append、stream read、atomic replace、queue checkpoint | FATFS、路径约束、互斥、掉电恢复 |
| `cloud_transport` | request、stream callback、cancel | TLS、证书、chunked、SSE、重试、限流 |
| `llm_provider` | chat stream、tool delta、cancel | SiliconFlow/DeepSeek 请求映射、错误归一化 |
| `agent_runtime` | start turn、cancel、snapshot | 消息循环、工具轮次、上下文预算 |
| `memory_engine` | recall、record、forget、maintain | SD 日志、索引、排序、TTL、墓碑、压缩、恢复 |
| `scheduler` | add、update、cancel、list | RTC、持久化、重启恢复、missed-run |

`memory_engine` 当前只有一个板载生产实现，不预先制造 provider/adapter 层。它自身是一个深模块：agent 只看到四类稳定操作，文件格式、索引和恢复策略保持局部。`llm_provider` 则是实际 seam，因为 SiliconFlow 与 DeepSeek 都是明确的生产实现。

### 3.3 运行位置

| 位置 | 必须承担 | 可选承担 |
|---|---|---|
| ESP32-S3 | agent loop、状态、权限、当前会话、语音、设备工具、Cron | Web Search、HA、Telegram |
| SiliconFlow | ASR、TTS | LLM、记忆候选提炼、可选 Embedding |
| DeepSeek | 无 | LLM、记忆候选提炼 |
| Companion | 无 | Browser、完整 MCP、Python/shell、多消息平台 |

长期记忆不部署 Gateway、数据库服务或 NAS 进程。LLM API 不可用时，设备仍保留记忆浏览/检索、当前会话、本地工具和定时任务；只有需要模型生成的新回答和自动记忆提炼暂停。Companion 仅用于 L6 重型能力，不参与 L1-L4 主链路。

## 4. 语音状态机

```text
IDLE
  -> LISTENING       唤醒成功，AFE/VAD 采集一句话
  -> TRANSCRIBING    WAV 写 SD，调用 ASR
  -> THINKING        调用 LLM
  -> TOOL_RUNNING    执行工具，可返回 THINKING
  -> SYNTHESIZING    请求 TTS
  -> SPEAKING        PCM 流式播放
  -> IDLE
```

异常和取消统一回到 `IDLE`。不为网络失败增加弹窗，只在语音页显示短状态。

首版采用半双工：

- 播放期间不监听新的唤醒词。
- BOOT 长按取消当前 ASR、LLM、工具等待或 TTS。
- 播放中打断和全双工 AEC 放到 P6 打磨阶段。

所有异步回调携带 `turn_id`。取消时关闭当前 HTTP client、停止 I2S、清空缓冲并递增 `turn_id`；旧回调因编号不匹配直接丢弃。

## 5. 云 API 链路

### 5.1 接口选择

| 阶段 | Provider/接口 | 首版策略 |
|---|---|---|
| ASR | SiliconFlow `POST /v1/audio/transcriptions` | 完整 WAV multipart 上传 |
| LLM | SiliconFlow 或 DeepSeek `POST /v1/chat/completions` | OpenAI-compatible JSON、SSE、Function Calling |
| TTS | SiliconFlow `POST /v1/audio/speech` | 16k、PCM、stream |

模型名称、base URL、超时和凭据引用全部配置化，不散落在业务代码。`llm_provider` 统一普通消息、流式增量、工具调用、取消和错误分类；`siliconflow_client` 与 `deepseek_client` 只负责协议差异。首版先接通一个 LLM provider，另一个作为配置切换和故障迁移能力，不做同一轮自动双发。

文档中的候选模型只用于首次联调：

- ASR：`FunAudioLLM/SenseVoiceSmall`。
- LLM：SiliconFlow 可先验证一个带 Tools 标签的模型；DeepSeek 可验证 `deepseek-chat`，最终以板上 Function Calling 实测为准。
- TTS：`FunAudioLLM/CosyVoice2-0.5B`。

### 5.2 ASR

SiliconFlow ASR 当前是完整文件上传，不是实时流式识别：

1. AFE 输出 16kHz、mono、signed 16-bit PCM。
2. VAD 判断一句话结束。
3. 写入 `/sdcard/agent/tmp/utterance.wav`。
4. `esp_http_client` 从 SD 分块构造 multipart 请求。
5. 解析完整 `text`，交给 `agent_runtime`。

单句首版限制 15 秒，原始 PCM 约 480KB。官方文档未明确列出全部输入格式，因此 P5.0 必须先实测设备生成的 16k mono WAV；验证失败后再评估编码器。

### 5.3 LLM 与 Function Calling

Chat 请求使用 `stream: true`。SSE parser 只处理：

- `delta.content`
- `delta.tool_calls`
- `finish_reason`
- `data: [DONE]`

`tool_calls[].function.arguments` 可能跨多个 SSE chunk，必须按 tool call 的 `index/id` 累积；只有收到完成原因后才解析和执行。每个 SSE 事件处理完立即释放 cJSON，禁止累积整份响应 DOM。

### 5.4 TTS

建议请求：

```json
{
  "model": "FunAudioLLM/CosyVoice2-0.5B",
  "input": "最终回答文本",
  "voice": "FunAudioLLM/CosyVoice2-0.5B:anna",
  "response_format": "pcm",
  "sample_rate": 16000,
  "stream": true
}
```

HTTP chunk 进入 PSRAM jitter buffer，达到起播水位后启动 I2S。首版等待最终回答完整后只请求一次 TTS，不做边生成边播和多句并行请求。

### 5.5 HTTPS 与失败处理

- 使用 ESP x509 certificate bundle，必须校验证书和主机名。
- API Key 通过 Bearer Header 发送。
- 分别处理 401、429、503、504 和连接超时。
- 429/503/504 本轮最多自动重试一次。
- 已执行副作用工具后不自动重放整轮请求。
- 失败时释放 HTTP、文件和音频资源，再返回 `IDLE`。

启用 ESP-SR、TLS 和证书包后，4MB app 分区是否足够必须由 P5.0 构建结果确认。

## 6. 设备端 agent loop

### 6.1 消息类型

```c
typedef enum {
    AGENT_ROLE_SYSTEM,
    AGENT_ROLE_USER,
    AGENT_ROLE_ASSISTANT,
    AGENT_ROLE_TOOL,
} agent_role_t;
```

每条消息包含：

- `role`
- `content`
- `tool_call_id`，非工具消息为空
- `timestamp`

模型 reasoning 不进入 TTS，也不写入长期会话。

### 6.2 执行循环

1. 将 ASR 文本追加为 `user` 消息。
2. 发送 system prompt、最近上下文和工具定义。
3. 返回普通文本时形成最终回答。
4. 返回 `tool_calls` 时拼接并校验参数。
5. 串行执行本地工具。
6. 追加 `assistant tool_call` 和 `tool result`。
7. 再次请求模型，直到得到最终回答。
8. 最终回答写入会话并交给 TTS。

硬限制：

- 一个用户轮次最多 4 次 LLM round-trip。
- 单轮最多 4 个 tool call。
- 首版所有工具串行执行。
- 参数和结果均有字节上限。
- 超过限制时停止执行，以固定短提示结束。

## 7. 工具设计

工具静态注册：

```c
typedef esp_err_t (*agent_tool_fn_t)(const cJSON *args, cJSON **result);

typedef struct {
    const char *name;
    const char *description;
    const char *parameters_json;
    bool mutating;
    agent_tool_fn_t execute;
} agent_tool_t;
```

模型参数不可信。每个工具必须自行校验类型、必填字段、长度、枚举和数值范围。

### 7.1 第一批只读工具

| 工具 | 数据来源 | 说明 |
|---|---|---|
| `get_device_status` | RTC、电池、Wi-Fi、SD 快照 | 固定字段 |
| `get_environment` | 当前温湿度和历史聚合 | 不返回原始长序列 |
| `get_weather` | 现有 weather cache | 缓存无效时返回不可用 |

`get_weather` 不重复请求天气 API；天气缓存由 `weather_client` 更新(2026-07-15起设备直连QWeather，不再经Mac `bridge`)。

### 7.2 个人智能体工具

在第一批工具稳定后再加入：

- `save_note(text)`：保存备忘到 SD。
- `search_notes(keyword)`：关键词检索，限制命中数。
- `set_timer(seconds, label)`：设备本地计时。
- `web_search(query, max_results)`：结构化搜索并保留来源。
- `web_extract(url, max_chars)`：调用正文提取 API，不在设备解析任意动态网页。
- `memory_search(query, limit)`：按需检索长期记忆，不把全部记忆注入 prompt。
- `ha_get_state` / `ha_call_service`：后期接入 Home Assistant 白名单实体。

不提供任意 shell、任意文件路径、任意 URL、删除数据和固件更新工具。

`mutating=true` 工具按风险处理：保存备忘、调音量可直接执行；控制外部设备、删除数据等高风险操作必须显示摘要并等待物理按键确认。system prompt 不是权限边界。

### 7.3 Web Search 与 Browser

联网不等于具备 Web 搜索。SiliconFlow 负责模型推理，搜索能力由工具补充：

```text
web_search(query, max_results, time_range)
web_extract(url, max_chars)
```

首版选择返回精简 JSON 的 Search provider，只取最多 5 条结果；正文提取使用 Firecrawl、Tavily、Exa、Jina 等远程服务之一，并限制为 8-16KB。所有网页内容标记为不可信数据，不能直接改变 system prompt 或绕过工具确认。

设备端必须阻止 localhost、局域网和私有 IP URL，限制重定向、下载大小和超时，防止 SSRF。真正需要 JavaScript、登录、点击、表单和截图的 Browser 通过云浏览器或 Companion 提供，不在 ESP32 运行 Chrome/Puppeteer。

Pi 生态的 `pi-web-access` 值得借鉴“三工具接口、provider fallback、结果缓存和引用”；`badlogic/pi-skills` 的 Brave Search Skill 可借鉴参数与渐进披露，但 TypeScript、DOM、Puppeteer 和本地浏览器实现不移植。

### 7.4 SD Skills 与 Pi 扩展兼容

设备采用 Pi/Agent Skills 的声明式目录：

```text
/sdcard/agent/skills/
  environment-report/SKILL.md
  daily-assistant/SKILL.md
  web-research/SKILL.md
```

启动只读取 `name`、`description`、版本、所需工具和内容哈希；模型命中技能后再加载正文。Skill 不携带可执行代码，只能引用已经注册的工具。

参考现有 Pi 项目的方式如下：

| 项目 | 借鉴内容 | 设备端处理 |
|---|---|---|
| `badlogic/pi-skills` | Skills 目录与 Web/Browser 使用说明 | 复用文本约定，执行层重写 |
| `pi-web-access` | Search/Extract、fallback、引用 | 实现精简 REST adapter |
| `pi-hermes-memory` | policy-only 注入、按需 search、纠错学习 | 只借鉴行为，由板载 `memory_engine` 重写 |
| `pi-schedule-prompt` | job schema、启停、一次/周期/Cron | RTC + SD 重写 |
| Tau `homeassistant-ops` | plan-confirm-apply、实体白名单 | 固定 HA REST 工具 |
| `pi-chat` / `pi-telegram` | 配对、channel:user 会话路由 | 首版只做 Telegram 单通道 |
| `pi-mcp-adapter` | 单代理工具、延迟发现、输出预算 | 后期固定 HTTP MCP 子集 |

第三方 Pi 项目只作为接口和安全策略参考。引入任何 Skill 前固定来源与版本、审查内容、记录哈希；不得将 pi.dev Gallery 视为可信商店。

## 8. 当前会话与板载长期记忆

长期记忆的事实数据、画像、索引、排序、遗忘、压缩和恢复全部运行在 ESP32-S3，并保存在 SD 卡。SiliconFlow 或 DeepSeek 只可帮助把对话提炼成结构化候选；它们不是记忆数据库，API 不可用也不能影响已有记忆的本地检索。

### 8.1 开源方案取舍

| 方案 | 为什么不直接部署到 ESP32-S3 | 借鉴内容 |
|---|---|---|
| Mem0 | Python、embedding 与 vector store 依赖 | 记忆抽取、用户/会话命名空间 |
| Letta | Python，SQLite 方案仍依赖 `aiosqlite/sqlite-vec` | core/archive memory、上下文预算 |
| Zep/Graphiti | Python、图数据库、LLM 与 embedding 栈 | 时间有效性、来源和 supersede |
| LangMem | Python、LangGraph Store，生产存储偏服务端 | 主动记忆工具、后台提炼 |
| Hermes / `pi-hermes-memory` | Python 或 Node.js、SQLite FTS5 | 小画像注入、历史按需搜索、纠错学习 |

这些项目适合作为行为和数据模型参考，不能因为有 SD 卡就直接移植运行时。本项目用 ESP-IDF/C 重写一个小 interface、深实现、可恢复的 `memory_engine`。

### 8.2 记忆分层

| 层 | 内容 | 位置 | 使用方式 |
|---|---|---|---|
| 工作记忆 | 最近 6-10 轮、当前工具状态 | PSRAM + 当前会话日志 | 当前轮直接使用 |
| 用户画像 | 姓名、回答偏好、稳定习惯、隐私设置 | SD A/B 槽，约 1-2KB | 每轮或按场景注入 |
| 语义记忆 | 偏好、纠正、稳定事实 | SD 追加日志 + 本地索引 | 查询相关项，预算 2-4KB |
| 情节记忆 | 会话摘要、事件、提醒完成记录 | SD 日志 + session archive | 先摘要，必要时回看来源轮次 |
| 程序性记忆 | 经审核的 `SKILL.md` | SD `skills/` | 命中后渐进加载 |
| 计划记忆 | reminder/cron/job | SD `jobs/` | 由 scheduler 独立恢复 |

原始会话可以归档，但不能把每一轮自动提升为长期事实。长期记忆只保存经过本机策略筛选的稳定小集合，避免模型误判、隐私泄漏和索引无限增长。

### 8.3 `memory_engine` interface

```c
esp_err_t memory_recall(const memory_query_t *query,
                        memory_context_t *out);
esp_err_t memory_record(const memory_event_t *event);
esp_err_t memory_forget(memory_id_t id);
esp_err_t memory_maintain(uint32_t budget_ms);
```

模块内部隐藏文件布局、版本迁移、CRC、A/B manifest、索引、排序、TTL、tombstone、supersede、压缩、画像更新和断电恢复。`agent_runtime` 不直接读写记忆文件，模型也不能传入路径。当前只有一种生产实现，因此不增加多实现抽象层；测试通过同一公共 interface 注入受控存储后端即可。

建议记录结构：

```json
{
  "v": 1,
  "id": "m_01...",
  "type": "preference",
  "content": "用户偏好简洁的中文回答",
  "keywords": ["中文", "简洁", "回答风格"],
  "importance": 0.8,
  "confidence": 0.95,
  "created_at": 1784080000,
  "updated_at": 1784080000,
  "expires_at": null,
  "source": {"session": "s_...", "turn": 18},
  "supersedes": null,
  "crc32": "..."
}
```

首版类型固定为 `profile/preference/fact/correction/episode`。模型输出只是候选，设备必须校验类型、长度、数值范围、来源和重复项；更新通过新记录 `supersedes` 旧记录，不原地覆盖事实。

### 8.4 SD 布局与掉电恢复

```text
/sdcard/agent/
  profile/
    profile_a.md
    profile_b.md
    manifest_a.bin
    manifest_b.bin
  memory/
    events-0001.jsonl
    tombstones.jsonl
    index/
      manifest_a.bin
      manifest_b.bin
      terms-0001.bin
      postings-0001.bin
    vectors/
  sessions/
    current.jsonl
    archive/
  skills/
  jobs/
  tmp/
    utterance.wav
```

可靠性规则：

1. 单一 `storage_task` 串行处理 SD 写入，录音、会话和记忆通过有界队列提交。
2. 事实先追加到 `events-*.jsonl`，每条带版本和 CRC；一轮结束后批量 `fflush + fsync`，不对流式 token 逐次同步。
3. 索引后更新；失败只标记 `index_dirty`，不能回滚已经落盘的事实。
4. 新索引在旁路 generation 构建并校验，再切换 A/B manifest；索引损坏时可从事实日志完整重建。
5. profile、manifest 和索引压缩都使用 generation + CRC，不仅依赖 FATFS `rename()` 的原子性假设。
6. SD 不可用时退化为 PSRAM 短会话；已有 agent 仍能使用无持久化的本地工具，但明确提示记忆暂不可写。

会话记录只保存 `role/content/tool_call_id/timestamp/crc32`，不保存 reasoning、API Key 和默认原始录音。发送给 LLM 的上下文始终是画像、相关记忆和最近 6-10 轮的受限组合，不读取整份 SD 历史。

### 8.5 中文本地召回

首版不引入 Jieba、向量数据库或知识图谱。中文按 Unicode code point 生成字符 bigram/trigram，英文和数字保留规范化 token；只索引提炼后的长期记忆与会话摘要，不索引全部原始 token。

召回流程：

1. 对 query 做规范化并生成相同的 n-gram。
2. 从 postings 取得最多 64 条候选。
3. 计算 BM25 风格 lexical score。
4. 叠加 importance、confidence、recency 和 memory type 权重。
5. 应用 tombstone/supersede，过滤过期和旧版本。
6. 去重后取 3-8 条，压入 2-4KB 的 prompt 预算。

字符 n-gram 对中文人名、型号、缩写和口语新词更稳，也不需要大词典。代价是 postings 较多，因此必须限制索引对象、候选数和单条长度。P6.2 先用真实中文偏好、纠正、过期和冲突样本建立 recall evaluation；没有评测收益前不加更重方案。

### 8.6 写入、遗忘与隐私

写入入口：

- 用户明确说“记住……”或调用 `remember(content, ttl)`，生成高优先级候选。
- 对话结束后可异步请求当前 LLM provider 输出固定 JSON，只提取稳定偏好、纠正、事实和重复习惯。
- 本地设备事件可直接写结构化 episode，不必调用模型。

设备侧才拥有最终决定权：执行 schema 白名单、来源绑定、长度和单日写入上限、重复检测、敏感项过滤。API Key、密码、token、身份证号、网页指令、模型臆测和中间推理默认不进入长期记忆。LLM 提炼失败只跳过本轮自动候选，不影响回复与已有记忆。

对外工具固定为：

```text
remember(content, ttl)
memory_search(query, limit)
list_recent_memories(limit)
forget_memory(id)
```

删除先追加 tombstone，立即从召回中隐藏，物理空间由 `memory_maintain(budget_ms)` 分片压缩回收。批量遗忘先列出候选，再由 BOOT 确认；维护任务必须避开录音、TTS 和屏幕刷新高峰。

### 8.7 SQLite 与可选语义向量

SQLite 在 ESP32-S3 + SDMMC 上可以通过第三方移植运行，但不作为首版主存：现有端口常见配置关闭同步、WAL 和线程安全，FTS5 也需要额外编译启用。只有 ESP-IDF 当前版本构建、固件/RAM 余量、FTS5 中文索引、10k 记录、并发和掉电故障注入全部通过，才考虑替换自建索引；否则 append-only 事实日志的恢复边界更清晰。

如果 n-gram 在真实评测中明显不足，可调用 SiliconFlow Embedding API 生成 128 维向量，在设备端归一化并量化为 int8 后只存 SD。检索仍由 ESP32 分块读取并做点积，首版不做 HNSW；向量文件必须记录 `model_id/dimension/normalization/quantization_version`。断网时停生成新向量并回退本地 n-gram，已有记忆仍可召回。DeepSeek API 当前只作为 LLM provider，不假设它提供 embedding。

## 9. UI 与中文字库

当前中文字库只包含源码中的静态文案，无法显示任意 ASR/LLM 中文。MVP 的语音页只显示固定状态：

- 待机
- 聆听
- 识别
- 思考
- 执行
- 播报
- 网络不可用

完整转写和回答通过语音呈现。后续如需显示任意中文，再设计 SD 字库包和按码点索引读取，不把完整 CJK 字库编进 `font_cn16.c`。

### 9.1 三个物理按键的真实边界

板上有三个物理按键，但只有两个可由固件读取：

| 按键 | 硬件 | 长期映射 |
|---|---|---|
| KEY | GPIO18 | 短按下一页；长按手动唤醒/进入聆听 |
| BOOT | GPIO0，低电平且是下载启动脚 | 短按页面动作/确认；长按返回/取消 |
| PWR | 硬件电源开关，不接 GPIO | 只负责开关机，固件不能分配功能 |

上下文规则：

- 待机时 KEY 长按是唤醒词之外的可靠入口，适合嘈杂环境。
- 智能体运行时 BOOT 长按统一取消 ASR、LLM、工具等待或 TTS。
- 高风险工具等待确认时，BOOT 短按确认、BOOT 长按拒绝。
- KEY 短按始终保留翻页，避免不同页面交互含义漂移。
- 不实现双键组合，除非后续出现无法用现有手势表达的真实需求。

当前 `ui_manager` 直接轮询 GPIO，KEY 只按下沿触发，没有稳定时间消抖，也没有 KEY 长按。P5 基础治理阶段将手势识别移入 `input_manager` 深模块；UI 和 voice 模块只接收 `INPUT_NEXT/ACTION/BACK/VOICE/CANCEL` 等语义事件。BOOT 在上电复位时仍承担下载模式功能，不能要求用户开机时按住。

### 9.2 交互一致性

现有页面底部只提示 KEY 翻页，但 BOOT 短按实际上会刷新页面。后续统一 footer：

```text
KEY 翻页 | 长按说话 | BOOT 操作
```

进入确认状态时临时替换为：

```text
BOOT 确认 | 长按取消
```

页面不自行解释物理 GPIO，只根据语义输入和 agent 状态渲染提示。

## 10. 密钥与安全

设备直连云端意味着模型和联网工具的 API Key 必须存放在设备：

- SiliconFlow、DeepSeek、Search、Telegram/HA Key 均不进 Git、不硬编码、不放 SD。
- 配网或串口配置后写入 NVS。
- 开发时使用独立、低额度、可吊销的 Key。
- 正式设备启用 NVS encryption、Flash Encryption、Secure Boot v2。
- HTTPS 不允许关闭证书校验。
- 默认不保留录音；调试录音需要显式开关和自动过期。

如果无法接受设备丢失后的密钥提取风险，可让 `bridge` 代持 Key。该模式只替换对应云 client 的请求目标，agent loop、SD 会话和长期记忆仍在设备。

现有 `bridge/server.py` 绑定 `0.0.0.0` 且没有认证，只适合当前局域网天气/额度读取；在加入远程工具或设备控制前，必须增加 Bearer/mTLS、请求大小限制和明确的网络暴露策略，或新建独立 Companion。不得直接把现有端口映射到公网。

网页、长期记忆和消息通道返回的文本都属于不可信外部数据。它们可以作为 tool result 供模型参考，但不能修改 system prompt、注册新工具、提升权限或跳过物理确认。

## 11. 现有功能审查与基础优化

P0-P4 已真机验证，长期规划不推翻现有功能，但在叠加语音、TLS、搜索和记忆前应先偿还以下结构性问题。

### 11.1 必须先修

| 优先级 | 现状与证据 | 优化方案 | 原因 |
|---|---|---|---|
| P0 | `net_manager.c` 在 WiFi 事件回调中 `vTaskDelay(10000)` | 用 timer/独立重连任务做慢速退避 | 默认 event loop 不能被阻塞 10 秒 |
| P0 | SNTP 只在启动后 15 秒内连网成功时启动 | 订阅首次 `GOT_IP`，幂等启动 SNTP | 晚连网设备当前可能一直不校时 |
| P0 | 规范依赖 `sys_state`，仓库却没有实现 | 先建立状态快照、命令、事件三个小 interface | 页面、工具、语音都需要统一状态 |
| P0 | `app_main.c` 已 99 行，达到 100 行红线 | 提取 `app_runtime` 管理启动顺序和功能注册 | P5 无法继续向 main 堆初始化 |
| P0 | KEY 无稳定时间消抖，KEY 长按未利用 | 抽出 `input_manager` | 语音、确认、取消需要一致输入语义 |

### 11.2 P5 同期优化

| 优先级 | 现状与证据 | 优化方案 |
|---|---|---|
| ~~P1~~ | ~~`page_home.c` 直接读 SHTC3/电池；`ui_pages` 依赖几乎所有业务模块~~ | 已被R0解决：四页(home/env_monitor/quota/weather)全部改读`sys_state` snapshot，`page_diag.c`保留为反面参照(未注册进`ui_manager`) |
| P1 | `display_st7305.c` 445 行，超过 400 行红线 | 保持一个公共 interface，在模块内部拆 panel/draw/text 文件 |
| ~~P1~~ | ~~weather/quota 重复 mDNS、HTTP 缓冲和解析骨架~~ | 已部分解决：`weather_client`已改设备直连QWeather(经`cloud_transport`)，不再和`claude_quota_client`共享mDNS+HTTP骨架；`claude_quota_client`仍自己实现，目前只有它一个消费方，暂不评估单独抽象 |
| ~~P1~~ | ~~`storage_sd` 只有 append line，没有互斥、目录、流读、原子替换~~ | 已被R0解决：Interface深化为append/read_chunk/atomic_replace/checkpoint，内部互斥锁串行化+路径校验 |
| ~~P1~~ | ~~weather 页面注释仍写"设备直连官方 HTTPS"，城市配置说明也已过时~~ | 已解决(且已经名副其实)：`weather_client`现在确实设备直连HTTPS，注释已同步 |
| ~~P1~~ | ~~quota/weather getter 解锁后再次读共享字段~~ | 已解决：`claude_quota_client_get()`/`weather_client_get()`都改成锁内计算`has_data`再解锁返回 |

### 11.3 后续产品化优化

- 后台轮询改用 `vTaskDelayUntil` 或事件触发，减少漂移和无效唤醒。
- 语音工作时推迟天气/额度轮询，避免 TLS、PSRAM 和 WiFi 带宽争用。
- 环境 CSV 按月轮转，提供聚合查询，不让 agent 扫描无限增长文件。
- 测量 RLCD 全帧刷新频率和功耗，再决定局部刷新，不提前改驱动。
- 当前分区没有 OTA A/B 槽。P5.0 测得 app 与 ESP-SR 模型真实尺寸后，必须决定继续有线升级，还是重新规划 signed OTA；不能在尺寸未知时拍脑袋改分区。
- 为纯逻辑模块增加 host/Unity 测试，测试通过同一个公共 interface，不测试内部实现。

基础治理只做行为保持的结构调整和确定性缺陷修复，不借机重写已真机稳定的 P0-P4。

## 12. 长期实施顺序

### R0 基础治理

范围：

1. 修复 WiFi event loop 阻塞和延迟联网不启动 SNTP。
2. 实现 `app_runtime`、`sys_state`、`input_manager`。
3. 深化 `storage_sd`，拆分显示模块内部文件。
4. 为 JSON/SSE、agent loop、input gesture 和 `memory_engine` 建立可测试 interface。
5. 记录 P0-P4 行为基线，逐项回归真机页面。

退出条件：P0-P4 行为不退化；`app_main.c` 回到充足余量；页面不再直接读传感器；两个按键手势稳定。

### P5.0 云 API 与资源验证 ✅已完成并真机验证(2026-07-15，详见CLAUDE.md"P5.0 完成记录")

暂不接麦克风，用固定文本和测试 WAV 验证：

1. 16k mono WAV 能被 ASR 接受。✅ SiliconFlow正确识别内置测试WAV语音内容
2. 选定的首发 LLM provider 普通 SSE 回答可解析，并能只改配置切换 SiliconFlow/DeepSeek。
   ✅ SiliconFlow已验证；⚠️ DeepSeek API Key暂缺，`deepseek_client`按同协议实现且编译通过，
   未做真实调用验证，切换只需改`app_main.c`一行`use_deepseek_llm`
3. 两个 provider 分别用一个只读工具完成两次 LLM 请求，确认工具增量格式已被归一化。
   ✅ SiliconFlow完整验证(工具调用→执行→结果回填→最终回答闭环)；DeepSeek同上未验证
4. TTS raw PCM 的声道、位宽、端序和 chunked 播放正确。✅ 已验证跨网络分片字节对齐拼接
5. TLS 开启后的固件体积、内部堆和 PSRAM 余量可接受。✅ +6.5%固件体积，堆/PSRAM无显著下降；
   ESP-SR尚未接入，其体积影响留给P5.1实测

验收：ASR、LLM、TTS 三类 API 分别跑通；SiliconFlow/DeepSeek LLM 可配置切换；断网、401、429、503、超时和取消后均能释放资源。

### P5.1 本地音频链路

1. 复用官方 demo 已验证的 ES7210/ES8311 时钟和 TDM 配置。
2. 实现 I2S 录放、AFE、WakeNet、VAD。
3. 生成临时 WAV。
4. 验证半双工取消。

验收：连续唤醒 20 次无任务泄漏；录音无明显截头；播放无爆音。

### P5.2 单轮语音问答

连接 `Wake -> ASR -> 单轮 Chat -> TTS`，加入语音状态页和网络降级。

验收：普通问答连续 20 轮；断网不重启、不死锁，恢复后可再次唤醒。

### P6.0 设备端智能体

加入 Pi 风格 agent loop、三个只读工具和 SD JSONL 会话。

验收：设备状态、环境和天气问题触发正确工具；非法参数不执行；工具循环不会无限运行；重启后能恢复最近上下文。

### P6.1 Web 与 Skills

1. 实现 `web_search`，保留来源 URL。
2. 实现云端 `web_extract`，加入 SSRF、长度和超时限制。
3. 实现 SD Skill 索引、按需加载、版本与哈希校验。
4. 首批 Skill：环境报告、每日助手、Web Research。

验收：能完成“搜索三个来源 -> 抽取两篇正文 -> 带来源总结”；恶意网页不能注册工具或绕过确认；拔掉 SD 后普通 agent 仍可工作。

### P6.2 板载长期记忆

按小步可验收顺序实现：

1. M1 可靠日志：`memory_engine` 四接口、单 storage task、profile A/B 槽、event JSONL + CRC；只支持明确 remember/list/forget。
2. M2 本地召回：字符 bigram/trigram、postings、BM25 风格排序和 2-4KB 上下文预算。
3. M3 生命周期：LLM 异步提炼候选，本机 schema/隐私/重复校验，加入 TTL、supersede、tombstone 和分片 compaction。
4. M4 语义实验：只对 n-gram 评测失败样本测试 SiliconFlow 128 维 embedding + 本地 int8 向量，不达成显著收益就不合入主线。
5. M5 容量维护：设定记忆条数/字节上限、会话归档、低价值衰减和用户导出。

验收：跨重启能回忆已确认偏好；撕裂末行可忽略；删除整个 index 后能从事实日志重建；被删除、过期或取代的记忆不再召回；自动提炼失败不阻断对话；10k 条压力测试中查询和维护不阻塞语音/UI。

### P6.3 主动助手

1. 实现 once/interval/cron 持久任务、重启恢复和 missed-run policy。
2. KEY 长按手动唤醒，BOOT 确认/取消。
3. 加入备忘、检索、计时器。
4. 根据真实需求二选一接入 Home Assistant 或 Telegram，不同时做两条通道。

验收：提醒任务掉电重启后仍存在且不会重复执行；高风险 HA 操作必须确认；消息通道完成配对、去重和会话隔离。

### P7 可选 Companion 增强

只有出现明确需求才部署 NAS/树莓派/VPS Companion：

- 云 Browser：JavaScript、登录、点击、截图。
- 固定白名单 HTTP MCP；完整 stdio MCP 留在 Companion。
- Python/shell、子智能体和多平台 Gateway。
- 统一代持不希望落在设备的第三方 Key。

ESP32 仍是身份、权限、会话和任务的权威，Companion 只是远程工具执行器。断开 Companion 不得破坏 L1-L4 核心能力。

### P8 产品化与升级

- 8/24/72 小时稳定性测试，覆盖掉网、限流、拔 SD、索引重建、压缩中断和取消。
- 根据实测资源决定 signed OTA 分区；启用 Secure Boot、Flash/NVS Encryption。
- 评估播放打断、SD 动态中文字库、局部刷新和功耗策略。
- 固定 Skill/Provider 版本、配置迁移和回滚格式。

退出条件：升级失败可恢复；凭据不出现在日志/SD；长期运行无持续堆下降；每个外部 provider 都有明确降级行为。

## 13. 开发期间必须记录

每个阶段记录：

- 固件大小和 4MB app 分区余量。
- 最小内部堆、最小 PSRAM。
- 各任务栈高水位。
- 唤醒到 ASR 文本的耗时。
- ASR 文本到 LLM 首 token 的耗时。
- TTS 首包和整轮耗时。
- 取消、断网和 SD 异常后的资源回收结果。

新增任务前，把栈大小和优先级登记到 `docs/coding-standards.md`。从官方 demo 摘取音频配置时，同步登记到 `docs/vendor-provenance.md`。

## 14. 主要风险

| 风险 | 处理 |
|---|---|
| Pi npm runtime 无法运行在 ESP32 | 只实现 Pi 风格最小语义 |
| ASR 非流式，停句后才上传 | 本地 VAD、15 秒上限 |
| SSE 工具参数被分片 | 按 index/id 累积，完成后再校验 |
| 模型不支持或不稳定调用工具 | 模型配置化，P5.0 做自检 |
| TTS 网络抖动造成断音 | PSRAM jitter buffer、达到水位再播放 |
| TLS、ESP-SR 导致 app 超过 4MB | P5.0 实测，必要时再调整分区 |
| 上下文和 cJSON 导致 OOM | 最近轮次、请求字节上限、增量解析 |
| SD 掉电损坏尾记录或索引 | JSONL、CRC、A/B manifest、索引可重建 |
| API Key 落在设备 | 加密 NVS、低额度 Key、可选 bridge 代理 |
| 任意中文无法显示 | MVP 固定状态，后续 SD 字库 |
| 中文 n-gram 索引膨胀 | 只索引提炼记忆/摘要，限制 gram 和候选数，分代压缩 |
| 自动提炼写入错误记忆 | LLM 只提议；本机 schema、来源、置信度、纠正覆盖 |
| 记忆无限增长或 SD 写放大 | TTL/上限/归档；按轮批量写；低优先级维护 |
| 压缩或索引切换中断 | 新 generation 旁路构建，CRC 校验后切换 A/B manifest |
| LLM provider 或 Search provider 不可用 | 本地记忆和工具继续；模型/搜索操作 fail-open 并明确降级 |
| 第三方 Pi Skill/Package 含恶意指令或代码 | 固定来源、版本与哈希；只加载声明式文本 |
| 网页 prompt injection / SSRF | 外部数据隔离、私网 URL 拒绝、工具权限不继承 |
| 多个后台网络任务抢占语音资源 | cloud transport 限流、语音优先、后台请求延后 |
| PWR 被误认为第三个可编程键 | 文档明确其为硬件开关，只设计 KEY/BOOT |
| 没有 OTA 分区阻碍长期升级 | P5.0 资源实测后做签名 OTA 决策门 |

## 15. 一手资料

- [Pi Agent Harness 官方仓库](https://github.com/earendil-works/pi)
- [Pi Agent Core：状态、事件和工具循环](https://github.com/earendil-works/pi/blob/dcfe36c79702ec240b146c45f167ab75ecddd205/packages/agent/README.md)
- [Pi Agent Core package.json：Node.js >= 22.19.0](https://github.com/earendil-works/pi/blob/dcfe36c79702ec240b146c45f167ab75ecddd205/packages/agent/package.json)
- [Pi Skills 文档](https://github.com/earendil-works/pi/blob/dcfe36c79702ec240b146c45f167ab75ecddd205/packages/coding-agent/docs/skills.md)
- [Pi Extensions 文档](https://github.com/earendil-works/pi/blob/dcfe36c79702ec240b146c45f167ab75ecddd205/packages/coding-agent/docs/extensions.md)
- [Pi Packages 文档与安全边界](https://github.com/earendil-works/pi/blob/dcfe36c79702ec240b146c45f167ab75ecddd205/packages/coding-agent/docs/packages.md)
- [Mem0 README 与运行时](https://github.com/mem0ai/mem0)
- [Letta README 与运行时](https://github.com/letta-ai/letta)
- [Graphiti README 与运行时](https://github.com/getzep/graphiti)
- [LangMem README 与运行时](https://github.com/langchain-ai/langmem)
- [Hermes Persistent Memory](https://hermes-agent.nousresearch.com/docs/user-guide/features/memory)
- [badlogic/pi-skills](https://github.com/badlogic/pi-skills/tree/90bb51cae36515a648515b633a81c0c6efc8c74d)
- [pi-web-access](https://github.com/nicobailon/pi-web-access/tree/7bdc30a65cf77273eb9c0034647b373bda4060d7)
- [pi-hermes-memory](https://github.com/chandra447/pi-hermes-memory/tree/bbd3217d7c98d2a81772a2505f7918c68be8978c)
- [pi-schedule-prompt](https://github.com/tintinweb/pi-schedule-prompt/tree/5556775276202c26654ff9323541fe6983f6ee38)
- [Tau homeassistant-ops](https://github.com/goncalossilva/tau/tree/2346b16b92679badf7962acd0a14d0efcf398b7a/skills/homeassistant-ops)
- [pi-chat](https://github.com/earendil-works/pi-chat/tree/9adbd29b40ee27ff1decf0fc87cbe180b40924f5)
- [pi-mcp-adapter](https://github.com/nicobailon/pi-mcp-adapter/tree/82724dccc13a49310530898f922bafff12b7f3fe)
- [SiliconFlow Chat Completions OpenAPI](https://docs.siliconflow.cn/cn/api-reference/chat-completions/chat-completions)
- [SiliconFlow Function Calling 指南](https://docs.siliconflow.cn/cn/userguide/guides/function-calling)
- [SiliconFlow Embeddings OpenAPI](https://docs.siliconflow.cn/cn/api-reference/embeddings/create-embeddings)
- [SiliconFlow Audio Transcriptions OpenAPI](https://docs.siliconflow.cn/cn/api-reference/audio/create-audio-transcriptions)
- [SiliconFlow Text to Speech OpenAPI](https://docs.siliconflow.cn/cn/api-reference/audio/create-speech)
- [DeepSeek Chat Completions API](https://api-docs.deepseek.com/api/create-chat-completion)
- [DeepSeek Function Calling 指南](https://api-docs.deepseek.com/guides/function_calling)
- [ESP-SR WakeNet 官方文档](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/README.html)
- [ESP-IDF HTTP Client](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/protocols/esp_http_client.html)
- [ESP-IDF ESP-TLS](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/protocols/esp_tls.html)
- [ESP-IDF 6.0.2 FatFS](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/api-reference/storage/fatfs.html)
- [ESP-IDF 6.0.2 SD/MMC 线程安全边界](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/api-reference/storage/sdmmc.html#thread-safety)
- [SQLite FTS5：trigram、BM25 与编译选项](https://www.sqlite.org/fts5.html)
- [ESP32-S3 SDMMC SQLite 示例](https://github.com/nopnop2002/esp32-idf-sqlite3/blob/main/sd_mmc/README.md)
- [ESP32 SQLite 移植编译配置](https://github.com/nopnop2002/esp32-idf-sqlite3/blob/main/components/esp32-idf-sqlite3/config_ext.h)
- [ESP-IDF NVS Encryption](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/storage/nvs_encryption.html)
- [ESP-IDF Secure Boot v2](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/secure-boot-v2.html)
