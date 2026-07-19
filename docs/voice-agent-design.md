# 设备端个人智能体：架构、边界与验收

> 最后更新：2026-07-18
> 当前基线：P5.2 核心语音闭环已真机验证；P6.0、P6.1、P6.2 M1、P6.3 M1 已实现。未完成的真机验收不视为已完成，见第 7 节。

本文件只保留当前实现、稳定约束和验收门槛。编码规则见 [`coding-standards.md`](coding-standards.md)，引脚见 [`hardware-pinmap.md`](hardware-pinmap.md)，摘取的供应商代码见 [`vendor-provenance.md`](vendor-provenance.md)。

## 1. 产品边界

设备是身份、状态、会话、记忆、任务和权限的唯一权威；云端仅提供 ASR、LLM、TTS 与受限公开网络检索；不得依赖 Mac/PC 常驻进程。

当前不做：任意代码或 shell 执行、动态安装工具、并行工具、多智能体、Cron、Home Assistant、Telegram、Browser、MCP 和远程执行。它们没有真实需求前不进入固件。

## 2. 实际架构

```text
main/app_main.c
      |
app_runtime                         启动顺序、注册、降级
      +-- input_manager             KEY/BOOT 的唯一 GPIO 所有者
      +-- sys_state                 跨模块状态、命令、提醒邮箱
      +-- ui_manager / ui_pages     只读快照并渲染
      +-- voice_assistant           一轮语音编排、取消、提醒播报
      |     +-- agent_runtime       消息 -> 模型 -> 工具循环
      |           +-- agent_session / agent_tools
      |           +-- skill_manager / memory_engine
      +-- scheduler                 一次/周期提醒、恢复、投递
      +-- 平台与能力模块
            +-- cloud_transport     TLS、GET、JSON、流 POST、取消、总超时
            +-- llm_provider        SiliconFlow / DeepSeek SSE
            +-- asr_client / tts_client
            +-- web_client          Jina Search / Reader，来源约束
            +-- storage_sd / 网络 / 音频 / 传感器 / 显示
                  |
              board_rlcd42
```

硬规则：页面不直接访问网络、音频或 SD；新增按键逻辑只进 `input_manager`；跨模块异步状态只经 `sys_state`；`board_rlcd42` 是唯一引脚与总线初始化所有者。`page_diag.c` 是未注册的旧式诊断页，不是新页面范例。

## 3. 运行时能力

### 3.1 语音与智能体

```text
MultiNet 命令词“贾维斯”
  -> AFE / VAD 录音
  -> SD WAV
  -> SiliconFlow ASR
  -> agent_runtime（最多 4 次模型往返、串行工具）
  -> 流式或兜底 TTS
  -> ES8311 扬声器
```

- `voice_assistant` 维护 `IDLE -> LISTENING -> TRANSCRIBING -> THINKING/EXECUTING -> SPEAKING -> IDLE`；异常和取消收敛回 `IDLE`。
- MultiNet 命令词是当前“贾维斯”入口；KEY 只短按翻页，BOOT 只用于真实确认或语音活动时的长按取消。
- 回复流在 TTS 前剥离 `*`、反引号、`#`、`_`；UTF-8 分块只提交完整字符前缀；PCM 分片始终按完整 `int16_t` 样本交付。
- TTS 复用同一 host 的 TCP/TLS 连接；零 PCM 或陈旧连接会关闭并重连一次。异常、取消和半截响应都丢弃连接，避免污染下一句。
- 传输层在每个上传分片和每轮读取前检查取消与 45 秒总预算；ASR 对非取消失败重试一次。

### 3.2 工具、Skill 与 Web

`agent_runtime` 只暴露固定 JSON Schema 工具；模型、网页和 Skill 都是不可信输入，不能新增工具、路径或权限。

- 常驻只读工具：设备状态、环境、缓存天气。
- `web_search` / `web_extract` 仅在 `JINA_API_KEY` 已配置时注册。搜索最多 5 个来源；抽取 URL 必须来自最近一次成功搜索，拒绝本地地址、字面 IP、用户信息和非 HTTP(S) scheme。
- Skill 位于 `agent/skills/<name>/SKILL.md`。front matter 只允许 `name`、`description`、`version`、`tools`、`content_crc32`；正文每次加载均复核 CRC32，工具必须是启动时的静态白名单子集。

### 3.3 长期记忆

`memory_engine` 只对外提供 recall、record、forget、maintain。SD 缺失时记忆工具不注册，普通短会话仍可运行。

- 事实日志：`agent/memory/events-0001.jsonl`；删除使用 `agent/memory/tombstones.jsonl`；每行有 CRC32，恢复时忽略 CRC 错误、重复 ID 和无换行的撕裂末行。
- 画像快照：`agent/profile/profile_a.json` 与 `profile_b.json` 双槽交替原子替换；最新有效 generation 胜出，事件日志仍是事实源。
- 热路径写入经 `memory_store_task` 的 8 深度 PSRAM 队列串行落盘。事件无法入队时不修改内存状态；事件落盘失败后停止更新画像快照，重启回放恢复一致性。
- 内存镜像上限 64 条，单条 240 字节；画像最多 6 条；TTL 只在可信 RTC/SNTP 时间下写入。

### 3.4 主动提醒

`scheduler` 只实现一次和周期提醒，最多 16 条：

- 任务表持久化为 `agent/tasks/tasks.json`；无 SD 时保留 RAM，重启后丢失但不影响对话。
- 时间不可信时不触发；一次任务过期少于 24 小时会补播，超过 24 小时丢弃；周期任务对齐到下一个周期，不补发历史周期。
- 到期文本先投递给 `sys_state` 的单槽提醒邮箱；语音空闲后停止唤醒监听、播报“提醒：…”，无论成功、失败或取消都恢复监听。
- `set_reminder` 与 `cancel_reminder` 只在用户明确表达提醒/定时意图时注册；`list_reminders` 是只读工具。

## 4. 配置与数据位置

- 本地凭据只放 `firmware/main/secrets.h`，模板是 `secrets.h.example`，不得提交密钥。
- `JINA_API_KEY` 为空时只禁用 Web 工具。
- 当前会话：`agent/sessions/current.jsonl`；无 SD 时退化为 RAM。
- 改动固件显示字符串或 `tools/charset.txt` 后必须运行 `python3 tools/gen_font.py`，不要手改 `font_cn16.c`。

## 5. 资源与降级原则

- 音频与大缓冲显式放 PSRAM 或 DMA 能力内存；网络、SD、模型服务失败必须返回错误或降级，服务层不得 `ESP_ERROR_CHECK`。
- 每 300 秒记录内部堆与 PSRAM 水位。天气在电池供电时降为 30 分钟轮询；USB 供电后自动恢复。
- `memory_store_task`、`scheduler_task`、临时 TTS 任务的栈、优先级以 `coding-standards.md` 的任务表为准。

## 6. 当前阶段状态

| 阶段 | 当前状态 | 已有证据 / 仍缺门槛 |
|---|---|---|
| P5.2 单轮语音闭环 | 核心完成 | MultiNet、音频、Wi-Fi、TLS、语音助手已多次启动；持续多轮、弱网与听感回归见第 7 节。 |
| P6.0 设备端 agent | 已实现 | 启动会恢复会话并注册本地工具；完整工具对话与跨重启会话回归待 SD 真机验收。 |
| P6.1 Web / Skills | 已实现 | 启动可索引 Skill；Jina 凭据、联网搜索、CRC 篡改和拔卡回归待验收。 |
| P6.2 记忆 M1 | 已实现 | 启动可恢复记忆并启动 `memory_store_task`；双槽、撕裂日志、tombstone 和连续写入待验收。 |
| P6.3 提醒 M1 | 已实现 | 启动可创建 `scheduler_task`；到点播报、取消、重启恢复和无 SD 降级待验收。 |

## 7. 未完成真机验收

以下项目未验证前不得在发布说明中称为“已完成”：

1. 多句回复中验证 TTS TLS 复用、陈旧 keep-alive 重连、Markdown 听感和 UTF-8 边界；弱网下确认 ASR 重试后仍能完成轮次。
2. 连续至少 20 轮语音，依据 `voice_assistant`、`tts_playback_task`、`reply_tts_task` 的高水位日志决定是否裁剪栈。
3. SD 记忆：记住/召回、重启恢复、双槽截断回退、JSONL 撕裂末行、遗忘 tombstone、拔卡降级及连续五轮写入无卡顿。
4. Web / Skill：配置 Jina Key 后执行搜索和来源受限抽取；验证 Skill CRC 篡改、拔卡和无 Jina Key 的独立降级。
5. 提醒：创建、到点播报、BOOT 长按取消、到期前重启、通话后补播、取消后不播以及拔 SD 后 RAM 降级。

## 8. 构建与发布

每次修改后，在 ESP-IDF 6.0.2 环境中运行 `idf.py build`；涉及行为的变更必须烧录并观察 `voice_assistant: 就绪`。提交前按 `coding-standards.md` 的清单检查依赖方向、任务登记、文件/函数体积、密钥和真机证据。
