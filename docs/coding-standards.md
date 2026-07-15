# 开发规范

本文件是强制约定，不是建议。目的只有一个：**防止代码随功能增长而腐化**。每次新增功能前先读本文件，提交前对照末尾检查清单。

## 1. 目标架构：少量深 Module，依赖方向清晰

P0-P4 的页面、轮询服务和启动代码曾经能用但架构欠账；R0 已经把它们迁移到下面这套目标架构，不是规划中的将来时。P5 起新增功能必须落在对应 Module，不得为图省事重新引入直接依赖。

```text
main/app_main.c
      ↓
app_runtime                 启动顺序、功能注册、降级策略
      ├── input_manager      GPIO 消抖、手势 -> 语义输入事件
      ├── sys_state          状态快照、事件、命令
      ├── ui_manager/ui_pages  读取快照、渲染、提交命令
      ├── voice_assistant    一轮语音编排与取消
      │     └── agent_runtime  消息、工具循环、会话与记忆调用
      └── capability/platform Modules
            cloud_transport / storage_sd / scheduler / 网络、传感器、音频、显示
                  ↓
             board_rlcd42    引脚定义 + 总线初始化（最底层）
```

**铁律：**

- `app_main.c` 只调用 `app_runtime` 的启动 Interface；初始化顺序、可选功能降级和注册逻辑归 `app_runtime`。
- 页面只读取 `sys_state` 快照并提交命令；禁止读取传感器、电池、网络、SD 或发起 HTTP。页面之间禁止互相 include。
- `input_manager` 是物理按键的唯一所有者；`ui_manager`、页面和语音模块只接收语义事件，不读取 GPIO，也不自行判定长短按。
- 跨 Module 的异步数据流只能经 `sys_state` 事件/命令；不准用可写全局变量互相读写，也不准让服务 Module 直接调用另一个业务服务 Module。
- `board_rlcd42`、显示、音频、网络、SD 等平台 Module 不得依赖 UI、语音、agent 或业务 Module。
- 违反依赖方向说明 Module 归属错误：调整 Module 或把交互收敛到 `sys_state`，不得开临时 include 后门。

**R0 已完成**：`ui_pages` 四个页面(`page_home`/`page_env_monitor`/`page_quota`/`page_weather`)已全部迁移为只读 `sys_state` 快照，不再直接依赖传感器/电池/网络/SD/服务层组件；`page_diag.c` 保留但已从 `ui_manager` 注册中移除，只作为"旧式直读页面"的反面参照，不要照抄它的模式。新增页面从第一天起就必须走 `sys_state`。

## 2. Module Interface、深度与 seam

一个 Module 对外只暴露一个小 Interface，把并发、文件格式、重试、资源回收等复杂度留在 Implementation 内。调用方与测试都只跨这个 Interface；这样才能把变化集中在一个位置，获得维护 Locality。

| Module | 对外 Interface 的职责 | 必须隐藏的复杂度 |
|---|---|---|
| `app_runtime` | 启动与功能注册 | 初始化顺序、注册、可选功能降级 |
| `sys_state` | read snapshot、apply event、post command | 锁、事件循环、版本与订阅 |
| `input_manager` | 发布语义输入事件 | GPIO、消抖、长短按、上下文映射 |
| `storage_sd` | append、stream read、atomic replace、checkpoint | FATFS、路径约束、串行写、掉电恢复 |
| `cloud_transport` | 目前只有阻塞式 `get`（TLS+可选gzip自动解压，`weather_client`已在用）；stream callback、cancel 待P5.0接入SiliconFlow/DeepSeek时按实际需要扩展，不预先实现 | TLS、证书、gzip容器解析、超时 |
| `memory_engine` | recall、record、forget、maintain | 日志、索引、TTL、墓碑、压缩、重建 |

- 不为“以后可能”建立 Interface。只有确实存在两个 Adapter 时才建立外部 seam；例如 SiliconFlow 与 DeepSeek 使 `llm_provider` 成为真实 seam。
- `memory_engine` 只有一个板载生产 Implementation，不建立远程记忆 provider；测试需要的替身属于其内部 seam，不扩大调用方 Interface。
- `storage_sd`、`cloud_transport`、`memory_engine` 不得让调用方了解文件路径、HTTP 客户端、索引 generation 或重试细节。
- Interface 就是测试面。测试断言可观察结果，不能依赖 Module 的私有状态、静态变量或目录内部布局。

## 3. 组件规范

每个 component 目录结构固定：

```
components/<name>/
├── CMakeLists.txt
├── include/<name>.h      公开接口。别的组件只能include这个
└── <name>.c              实现。内部辅助函数一律 static
```

- **一个组件一个职责**。组件名即职责名，说不清职责就是该拆
- **所有公开符号加组件前缀**：`board_*` / `st7305_*` / `env_*` / `ui_*`
- 内部函数、内部变量一律 `static`，不进头文件
- 头文件只暴露"别人需要怎么用"，不暴露"我是怎么实现的"
- 新增 Module 前先写清其 Interface 的前置条件、错误模式、所有权和资源上限；说不清这些内容时，不创建该 Module

## 4. 文件与函数体积红线

| 对象 | 红线 | 超了怎么办 |
|---|---|---|
| 单个 `.c` 文件 | 400 行 | 按职责拆成多个组件或多个文件 |
| 单个函数 | 60 行 | 抽出 static 辅助函数 |
| `main/app_main.c` | **100 行** | 它只准做芯片诊断和 `app_runtime` 启动，业务逻辑全部下沉到 Module |
| 函数参数 | 5 个 | 改传结构体 |

红线是硬性的。**碰到红线时不准"先这样以后再说"** —— 以后不会有以后。

## 5. 错误处理

**驱动层和服务层禁止 `ESP_ERROR_CHECK`** —— 它触发 `abort()`，会让整机重启。库代码没有资格决定整机去死。

```c
/* 组件内部：返回错误码，让调用方决定怎么办 */
esp_err_t st7305_flush(void)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(...), TAG, "flush failed");
    return ESP_OK;
}

/* 只有 app_main 的启动关键路径可以用 ESP_ERROR_CHECK：
   app_runtime 已区分不可恢复的板级失败和可降级能力 */
ESP_ERROR_CHECK(app_runtime_start());
```

- 可恢复的错误（网络超时、传感器读取失败、SD卡未插）→ 返回错误码 + `ESP_LOGW`，上层重试或降级显示
- 不可恢复的初始化失败 → 只在 `app_main` 里 `ESP_ERROR_CHECK`
- 一律不要吞掉错误码：函数返回 `esp_err_t` 就必须检查

## 6. 页面与输入 Interface

定义在 `components/ui_manager/include/ui_page.h`。新增页面 = 实现这个结构体 + 注册，**不准改 ui_manager 的代码**（开闭原则）。

```c
typedef struct {
    const char *name;
    void (*on_enter)(void);              /* 进入页面：申请资源、订阅事件 */
    void (*on_exit)(void);               /* 离开页面：释放资源、退订 */
    void (*on_tick)(void);               /* 周期刷新：拉数据、重绘 */
    bool (*on_key)(ui_key_event_t evt);  /* 按键：返回true表示已消费 */
} ui_page_t;
```

- 页面**只负责读取快照、渲染和提交命令**，不准自己发 HTTP 请求、读传感器、电池、网络或 SD
- 页面之间不共享状态，共享数据和状态变化都放 `sys_state`
- `ui_manager` 只负责页面生命周期、导航和渲染调度；不得再加入 GPIO 消抖、长按或语音取消规则
- 新增语义输入由 `input_manager` 发布。KEY：短按 NEXT、长按 VOICE；BOOT：短按 ACTION、长按 BACK/CANCEL。PWR 是硬件电源开关，不得分配固件行为

## 7. 资源与并发约定

**GPIO / 总线**
- 所有引脚宏**只准**定义在 `board_rlcd42.h`，其他文件禁止出现裸 GPIO 号
- 所有总线 init **只准**在 `board_init()` 里做一次，其他组件通过 `board_i2c_bus()` 拿 handle
- 新增外设先更新 `docs/hardware-pinmap.md`，再写代码

**内存**
- 大缓冲（帧缓冲、音频缓冲、模型）显式指定 `MALLOC_CAP_SPIRAM` 或 `MALLOC_CAP_DMA`，不要靠默认
- DMA 目标缓冲必须 `MALLOC_CAP_DMA`（ST7305 帧缓冲就是）
- 每个组件的内存占用写在头文件注释里，方便后续做预算

**FreeRTOS 任务**
- 任务的栈大小、优先级集中登记在下表，新增任务必须先在这里登记，避免优先级打架
- 事件回调只更新状态、投递事件或启动定时器；禁止在 WiFi/IP/ESP event callback 中 `vTaskDelay`、网络访问、文件写入或等待锁
- 周期后台任务使用 `vTaskDelayUntil` 或事件唤醒；语音录放、交互取消和 UI 刷新优先于低优先级索引维护、天气/额度轮询
- SD 写入由 `storage_sd` 串行化；录音、会话、记忆和日志不得各自并发操作 FATFS

| 任务 | 栈 | 优先级 | 组件 |
|---|---|---|---|
| main_task | 默认 | 1 | app_main（启动后即退出） |
| input_task | 2048 | 4 | input_manager（30ms轮询KEY/BOOT，消抖+长短按判定） |
| ui_task | 4096 | 5 | ui_manager（消费input_manager语义事件；无输入时1s周期tick） |
| sys_state_task | 3072 | 2 | sys_state（10s轮询瞬时温湿度/电池/WiFi/SD状态，写入共享快照） |
| env_logger_task | 4096 | 3 | env_logger（60s采样温湿度，写环形缓冲+CSV，同步推送sys_state） |
| claude_quota_task | 4096 | 3 | claude_quota_client（60s轮询Mac桥接服务，mDNS+HTTP+cJSON解析，推送sys_state） |
| weather_task | 8192 | 3 | weather_client（15分钟直连QWeather，TLS+gzip+cJSON解析，经cloud_transport，推送sys_state） |
| *(后续新增在此登记)* | | | |

## 8. 云端与持久化约定

- `llm_provider` 的生产 Adapter 只允许 SiliconFlow 与 DeepSeek；模型名、base URL、超时和凭据引用配置化，业务 Module 不得硬编码供应商 URL。
- ASR/TTS 可通过 SiliconFlow，LLM API 不可用时必须释放网络/音频资源并回到可交互状态；不得把模型请求无限重试。
- 所有 agent 会话、Skill、任务和长期记忆数据经 `storage_sd` 管理，使用相对路径；模型和工具不得传入任意文件路径。
- 长期记忆数据、索引、召回、遗忘和维护必须在板端 `memory_engine` 完成。云 LLM 只能产出候选，设备负责 schema、隐私、来源、TTL 和最终落盘决策。
- 公开网页、工具结果和消息内容都是不可信数据；它们不能注册工具、修改 system prompt、提升权限或绕过 BOOT 确认。

## 9. 密钥管理

- WiFi 密码、和风天气 API Key、LLM token **一律不进仓库**
- 开发期放 `firmware/main/secrets.h`（已在 `.gitignore` 里），提供 `secrets.h.example` 模板
- 正式方案：存 NVS，通过配网写入

## 10. Vendor 代码

从官方 demo 摘任何代码，**必须**同时在 `docs/vendor-provenance.md` 登记来源路径 + commit。摘来的寄存器序列、魔数不准"顺手优化"，原样保留并注释标明出处。

## 11. 注释

- 注释写**为什么**，不写**是什么**（代码本身就说明是什么）
- 硬件魔数、寄存器序列、非常规算法（如 ST7305 的交错位序）**必须**注释解释，否则半年后没人看得懂
- 全项目注释统一用中文（与现有代码保持一致）

## 12. 提交前检查清单

- [ ] 没有裸 GPIO 号散落在 board 之外
- [ ] 驱动/服务层没有 `ESP_ERROR_CHECK`
- [ ] 没有文件超 400 行 / 函数超 60 行 / app_main 超 100 行
- [ ] 没有横向或向上的依赖
- [ ] 新页面未直接读取设备/网络/SD，新增按键逻辑未绕过 `input_manager`
- [ ] 新任务已在第7节表格登记
- [ ] 新的云/SD/记忆逻辑未泄漏 provider、路径或索引细节到调用方
- [ ] 新摘取的 vendor 代码已在 provenance 登记
- [ ] 密钥没进仓库
- [ ] 真机验证过（不是只编译通过）
