# 开发规范

本文件是强制约定，不是建议。目的只有一个：**防止代码随功能增长而腐化**。每次新增功能前先读本文件，提交前对照末尾检查清单。

## 1. 分层架构：依赖只能向下

```
main/app_main.c            启动编排 + 页面注册。不写任何业务逻辑
      ↓
ui_pages/                  各功能页面，实现统一 page 接口
      ↓
ui_manager                 渲染框架、页面导航、状态栏
      ↓
服务层                      env_logger / weather_client / claude_quota_client
                           rtc_pcf85063 / storage_sd / net_manager
      ↓
驱动层                      display_st7305 / audio_pipeline / wakeword
      ↓
board_rlcd42               引脚定义 + 总线初始化（最底层，不依赖任何组件）
```

**铁律：**
- 只能依赖下层，**禁止向上依赖**（驱动层不准 `#include` 服务层）
- **禁止横向依赖**：页面之间不准互相 include，服务之间不准直接调用
- 跨层/跨模块通信一律走 `sys_state`（ESP-IDF event loop + 共享状态），不准用全局变量互相读写
- 违反依赖方向的需求，说明分层错了 —— 改分层，不要开后门

## 2. 组件规范

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

## 3. 文件与函数体积红线

| 对象 | 红线 | 超了怎么办 |
|---|---|---|
| 单个 `.c` 文件 | 400 行 | 按职责拆成多个组件或多个文件 |
| 单个函数 | 60 行 | 抽出 static 辅助函数 |
| `main/app_main.c` | **100 行** | 它只准做启动编排，业务逻辑全部下沉到组件 |
| 函数参数 | 5 个 | 改传结构体 |

红线是硬性的。**碰到红线时不准"先这样以后再说"** —— 以后不会有以后。

## 4. 错误处理

**驱动层和服务层禁止 `ESP_ERROR_CHECK`** —— 它触发 `abort()`，会让整机重启。库代码没有资格决定整机去死。

```c
/* 组件内部：返回错误码，让调用方决定怎么办 */
esp_err_t st7305_flush(void)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(...), TAG, "flush failed");
    return ESP_OK;
}

/* 只有 app_main 的启动关键路径可以用 ESP_ERROR_CHECK：
   板子初始化失败时，继续运行没有意义 */
ESP_ERROR_CHECK(board_init());
```

- 可恢复的错误（网络超时、传感器读取失败、SD卡未插）→ 返回错误码 + `ESP_LOGW`，上层重试或降级显示
- 不可恢复的初始化失败 → 只在 `app_main` 里 `ESP_ERROR_CHECK`
- 一律不要吞掉错误码：函数返回 `esp_err_t` 就必须检查

## 5. 页面接口契约（后续所有UI页面必须实现）

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

- 页面**只负责渲染和交互**，不准自己发HTTP请求/读传感器——数据从服务层拿
- 页面之间不共享状态，共享的东西放 `sys_state`

## 6. 资源约定

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

| 任务 | 栈 | 优先级 | 组件 |
|---|---|---|---|
| main_task | 默认 | 1 | app_main（启动后即退出） |
| ui_task | 4096 | 5 | ui_manager（30ms轮询按键，1s页面tick） |
| env_logger_task | 4096 | 3 | env_logger（60s采样温湿度，写环形缓冲+CSV） |
| claude_quota_task | 4096 | 3 | claude_quota_client（60s轮询Mac桥接服务，mDNS+HTTP+cJSON解析） |
| weather_task | 4096 | 3 | weather_client（15分钟轮询Mac桥接服务，mDNS+HTTP+cJSON解析） |
| *(后续新增在此登记)* | | | |

## 7. 密钥管理

- WiFi 密码、和风天气 API Key、LLM token **一律不进仓库**
- 开发期放 `firmware/main/secrets.h`（已在 `.gitignore` 里），提供 `secrets.h.example` 模板
- 正式方案：存 NVS，通过配网写入

## 8. Vendor 代码

从官方 demo 摘任何代码，**必须**同时在 `docs/vendor-provenance.md` 登记来源路径 + commit。摘来的寄存器序列、魔数不准"顺手优化"，原样保留并注释标明出处。

## 9. 注释

- 注释写**为什么**，不写**是什么**（代码本身就说明是什么）
- 硬件魔数、寄存器序列、非常规算法（如 ST7305 的交错位序）**必须**注释解释，否则半年后没人看得懂
- 全项目注释统一用中文（与现有代码保持一致）

## 10. 提交前检查清单

- [ ] 没有裸 GPIO 号散落在 board 之外
- [ ] 驱动/服务层没有 `ESP_ERROR_CHECK`
- [ ] 没有文件超 400 行 / 函数超 60 行 / app_main 超 100 行
- [ ] 没有横向或向上的依赖
- [ ] 新任务已在第6节表格登记
- [ ] 新摘取的 vendor 代码已在 provenance 登记
- [ ] 密钥没进仓库
- [ ] 真机验证过（不是只编译通过）
