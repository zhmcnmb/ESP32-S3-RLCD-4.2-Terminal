# CLAUDE.md

**写代码前必读 [`docs/coding-standards.md`](docs/coding-standards.md)** —— 分层依赖规则、文件体积红线、错误处理、页面 Interface 契约都是强制约定。硬件引脚的唯一权威来源是 [`docs/hardware-pinmap.md`](docs/hardware-pinmap.md)。P5-P8 的功能取舍、板载记忆和验收路线以 [`docs/voice-agent-design.md`](docs/voice-agent-design.md) 为准；R0 架构迁移和现有四页改版以 [`docs/r0-ui-v2-spec.md`](docs/r0-ui-v2-spec.md) 为准。

## 项目是什么

Waveshare ESP32-S3-RLCD-4.2 开发板固件，目标是打造一个桌面终端，集成：低功耗时钟+天气、语音交互(唤醒词+LLM对话)、环境监测记录(温湿度+SD打点)、Claude Code用量额度显示，并逐步升级为 agent runtime、会话、长期记忆和任务调度都运行在开发板上的个人智能体。模型计算允许使用 SiliconFlow 和 DeepSeek API。

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

R0已完成：`main → app_runtime → sys_state/input_manager/ui_manager+ui_pages/平台Module → board_rlcd42`
分层已落地，四个业务页面只读 `sys_state` 快照，不再直接依赖传感器/电池/网络/SD/服务组件。
P5起新增功能继续遵守这条依赖方向，不要为语音/agent开临时直读后门。

**已实现**

| Component | 职责 |
|---|---|
| `board_rlcd42` | 唯一的引脚定义与总线init(I2C/SPI/按键)，暴露handle与I2C扫描 |
| `display_st7305` | ST7305反射屏驱动：1bpp帧缓冲、画点/矩形、ASCII 8×8字体、**中文16×16字库(UTF-8混排)** |
| `input_manager` | KEY/BOOT消抖+长短按判定，唯一GPIO输入所有者，发布NEXT/ACTION/BACK/VOICE语义事件 |
| `sys_state` | 状态快照聚合：时间/瞬时温湿度/电池/WiFi/SD由内部轮询任务采集，环境历史/天气/额度由对应Producer推送；`sys_state_get_snapshot()`一次性一致读取 |
| `app_runtime` | 启动编排：硬件init→可选服务→UI注册→异步联网，接管原`app_main.c`全部业务逻辑，密钥经`app_runtime_config_t`传入(不直接依赖`secrets.h`避免组件循环依赖) |
| `ui_manager` | 页面注册、导航、tick调度（`ui_task`）；只消费`input_manager`语义事件，不读GPIO |
| `ui_pages` | `page_home`(桌面时钟+温湿度摘要)、`page_env_monitor`(温湿度趋势)、`page_quota`(Claude用量5h/7d)、`page_weather`(和风天气实况)，均只读`sys_state`快照(UI V2布局，见`docs/r0-ui-v2-spec.md`)。内部私有视觉原语`ui_v2.h/c`(共享页头/页脚/分隔线/空状态)与`ui_icons.h/c`(状态/天气图标)不对外暴露。`page_diag`(硬件自检)已从注册中移除，文件保留作为**旧式直读页面参照范例** |
| `sensor_shtc3` | SHTC3温湿度读取，内部互斥锁防多任务并发 |
| `rtc_pcf85063` | RTC读写(BCD转换)、掉电检测。**符号必须用全前缀**，见Gotchas |
| `net_manager` | WiFi STA连接、断线自动重连(慢速重试用`esp_timer`单次定时器，事件回调不阻塞)；`net_manager_register_connected_cb()`供首次GOT_IP一次性回调(SNTP启动用) |
| `time_service` | 启动从RTC恢复时间，联网后SNTP校时并写回RTC |
| `storage_sd` | SD卡挂载(SDMMC 1-bit + FATFS)；深化后Interface = append/stream read/atomic replace/checkpoint，内部互斥锁串行化+路径校验(拒绝`..`和绝对路径) |
| `battery` | 电池电压ADC(GPIO4,3倍分压,eFuse校准)，电量百分比估算 |
| `env_logger` | 后台任务60s采样SHTC3，环形缓冲(1小时窗口)+CSV落盘(经`storage_sd`)，每次采样后把历史整体推给`sys_state` |
| `claude_quota_client` | mDNS解析桥接服务主机名+HTTP轮询`/api/quota/compact`+cJSON解析，60s周期，结果推给`sys_state`(登记于coding-standards第7节) |
| `weather_client` | 设备直连QWeather `/v7/weather/now`（HTTPS+cJSON解析），15分钟周期，结果推给`sys_state`。经`cloud_transport`组件走TLS+gzip解压，原因见下方"P3 天气架构"一节 |
| `cloud_transport` | HTTPS GET(gzip自动解压)+流式POST(SSE/PCM响应逐块回调+取消)公共底座；weather_client/llm_provider/asr_client/tts_client共用 |
| `llm_provider` | SiliconFlow/DeepSeek统一chat接口，SSE解析+tool_calls增量归一化+429/503/504重试一次；provider由`llm_provider_init()`运行时选择 |
| `asr_client` | SiliconFlow语音转写，multipart/form-data上传SD卡WAV文件 |
| `tts_client` | SiliconFlow语音合成，PCM流式回调(跨网络分片字节对齐拼接) |

**待实现**

| Component | 职责 | Phase |
|---|---|---|
| `audio_pipeline` / `wakeword` / `voice_assistant` | ES8311/ES7210音频、ESP-SR唤醒、一轮语音编排与取消 | P5.1 |
| `agent_runtime` / `agent_session` / `agent_tool_registry` | Pi 风格消息/工具循环、SD 会话、工具权限与输出预算 | P6.0 |
| `memory_engine` / `scheduler` | 板载长期记忆与持久任务；数据、检索、维护全部在 SD/设备端 | P6.2/P6.3 |

**中文字库**：`tools/charset.txt` 维护字符集 → `python3 tools/gen_font.py` 生成 `font_cn16.c`。
源码字符串字面量里的中文会被`gen_font.py`自动扫描收录，**新增文案后只需重跑脚本**；只有运行时才出现、源码里没有字面量的文案(如天气API返回的现象词、风向)才需要手工写进`charset.txt`。缺字时屏幕显示空心框。当前244字约8.1KB(R0 UI V2改版新增13字)。

## 架构分层规则（R0已落地，强制维持）

`main → app_runtime → sys_state/input_manager/ui_manager+ui_pages/平台Module → board_rlcd42` 已经是当前代码的实际结构，不是待办。新代码必须遵守以下归属，不准开临时直读后门：

- `app_main.c`(35行) 只做芯片诊断和把`secrets.h`装进`app_runtime_config_t`传给`app_runtime_start()`；不直接初始化/注册任何业务 Module(`app_runtime`不依赖`secrets.h`本身，避免`main`↔`app_runtime`组件循环依赖)。
- 页面只调用一次 `sys_state_get_snapshot()` 取得整体一致快照、渲染；不能读传感器、电池、网络、SD，也不能自己起后台任务。R0尚未提供"post command"通道(现有4页BOOT ACTION都只是"用当前缓存重绘"，见`docs/r0-ui-v2-spec.md` Implementation Decisions)。
- `input_manager` 是唯一 GPIO 输入所有者。KEY 短按 NEXT、长按 VOICE；BOOT 短按 ACTION、长按 BACK(语义由消费方按上下文解读为"返回"或"取消")；PWR 无固件行为。`input_manager`的回调只入队，真正的绘制/dispatch只在`ui_manager`自己的`ui_task`里发生，避免多任务竞争帧缓冲/SPI。
- `storage_sd` 深化后 Interface = `append`/`read_chunk`/`atomic_replace`/`checkpoint`，内部互斥锁串行化+路径校验(拒绝`..`和绝对路径)；调用方只用相对路径，不接触 FATFS 细节。`checkpoint()`目前是空操作(当前所有写入都在单次调用内open+write+close，已经落盘)，留着Interface给未来批量写入模式用。
- `cloud_transport` 隐藏 TLS/SSE/取消；`llm_provider` 仅因 SiliconFlow 与 DeepSeek 两个实际生产 Adapter 而存在。不要为 `memory_engine` 建立远程 provider。
- `memory_engine` 保存、检索、遗忘和维护都在板端。LLM 只输出候选，设备验证 schema、来源、敏感信息、TTL 与最终落盘。

**新增页面的做法**：在 `ui_pages/pages/` 下新建`page_xxx.c`，实现 `ui_page_t`，只读 `sys_state_snapshot_t`，复用`ui_v2.h`(共享页头/页脚/分隔线/空状态)和`ui_icons.h`(图标)，在 `ui_pages.h` 暴露 getter，由 `app_runtime` 注册。**不要改 ui_manager**，也不要从页面直读设备或网络；`page_diag.c`保留作为"旧式直读"反面参照(文件头注释已说明，不要照抄它的直读模式)。

## R0 完成记录(架构治理 + UI V2)

范围：网络确定性修复 → `sys_state` → `input_manager` → `app_runtime` → `storage_sd`深化 → UI V2视觉原语 + 四页迁移，按`docs/r0-ui-v2-spec.md`第13节顺序实施，逐步真机验证(细节见该文档)。

**完成标准核对**(对照`docs/r0-ui-v2-spec.md`第14节)：
- ✅ 四页均不再直接读取传感器/电池/网络/SD，只读`sys_state`快照(已用`grep`确认四个page_*.c不再include旧direct-read头文件)
- ✅ `ui_manager`不再读GPIO/判长短按(GPIO消抖长短按已整体移到`input_manager`)
- ✅ `app_main.c`不含业务Module初始化和页面注册细节(精简到35行，只做芯片诊断+调用`app_runtime_start()`)
- ✅ WiFi事件回调不阻塞(`vTaskDelay`已替换为`esp_timer`单次定时器)；延迟联网SNTP能自动启动(`net_manager_register_connected_cb`，真机上多次真实断线重连场景都验证过)
- ✅ 四页共用一致页头(标题+SD/WiFi/电池图标)/页脚(KEY/VOICE/BOOT提示)/空状态(`ui_v2.h`)
- ⚠️ **无法远程验证**：四页在1米正常室内光线下的实际可读性、KEY/BOOT物理按键手势——本次由AI agent远程完成，没有摄像头/机械臂访问屏幕和按键；已用Python/PIL按真实坐标渲染四页布局mockup做视觉复核(无重叠/越界)，`input_manager`的GPIO持续轮询数分钟无故障，但**建议用户拿到实机后做一次目视确认+物理按键测试**作为最终验收
- ✅ 页面内容未变化时不做无意义整屏flush：quota/weather保持原有10s节流重绘(避免真机上每秒闪烁)，`env_monitor`保持30s节流；首页每秒刷新是有意为之(时钟本身每秒变化，不算无意义)
- ✅ P0-P4数据含义、页面顺序、BOOT刷新行为未退化：真机上quota/weather仍能通过bridge成功轮询拿到200，且额外测试了bridge主动关闭90秒+恢复的场景，`claude_quota_client`按预期报`ESP_ERR_HTTP_CONNECT`降级、bridge恢复后静默恢复正常轮询
- ✅ 新增中文均被字库收录：`tools/gen_font.py`自动扫描新增231→244字，无需手工登记(见下方字库小节)
- ✅ 固件大小/内存水位改版前后有记录：见下方数据

**固件体积与内存水位**(R0前后对比，R0前基线取自净网络修复后、深层架构改造前的构建)：

| 指标 | R0前 | R0后 | 差值 |
|---|---|---|---|
| 固件总镜像 | 1,112,779 字节 | 1,116,179 字节 | +3,400字节(+0.3%) |
| DIRAM占用率 | 37.06% | 37.96% | +0.9pp |
| 启动完成时堆剩余 | 未记录(R0前无该诊断) | 约8.56MB(历史最低约8.56MB) | 新增诊断 |
| 启动完成时PSRAM剩余 | 未记录 | 约8.38MB(历史最低约8.38MB) | 新增诊断 |

体积增量小的原因：R0大部分工作是把已有逻辑重新组织到新的Module边界(app_main→app_runtime、ui_manager内联GPIO→input_manager)，真正新增的代码(sys_state聚合/锁、storage_sd互斥+路径校验、UI V2页头页脚/天气图标)体量都不大。`esp_get_free_heap_size()`+`heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)`诊断已加进`app_runtime`启动完成日志，后续阶段可直接对比。**per-task栈高水位未采集**(需要开`CONFIG_FREERTOS_USE_TRACE_FACILITY`才能用`vTaskList()`，本次判断非阻塞性收益暂缓，留给P5.0做资源验证时一并开启)。

## P4 用量数据源（重要设计决定）

**开发环境本机用的是 OMP 框架（`omp` CLI）跑 Claude 订阅，不是官方 `claude` CLI**，因此`bridge/`原计划"解析`~/.claude/projects/**/*.jsonl`做近似估算"的方案**已废弃**。

实测确认：`omp usage --provider anthropic --json` 直接返回 Anthropic 官方(未文档化)OAuth用量接口的**权威**数据——`anthropic:5h`(Claude 5 Hour)与`anthropic:7d`(Claude 7 Day)两个窗口的`usedFraction`+`resetsAt`，与Claude Code自带`/usage`背后是同一接口。这份数据是**账号级**的，不区分客户端(官方`claude` CLI、OMP、claude.ai网页都算同一份用量)，所以不需要本机解析任何会话日志，也不需要"近似估算"标注。OMP自身会把结果写入`~/.omp/agent/agent.db`的`usage_history`表(带`resets_at`/`used_fraction`)做缓存。

`bridge/`桥接服务改为：定时(如60s)`subprocess`调用`omp usage --provider anthropic --json`并解析，替代原jsonl增量解析方案。详见`bridge/README.md`。

## P3 天气架构（2026-07-15由代理转发改为设备直连，决定已变更）

`weather_client` 现在**设备直连**QWeather，不再经`bridge/`代理。原设计（代理转发）的技术顾虑是：QWeather新版API Host（项目专属域名）**不管请求方Accept-Encoding怎么设都强制gzip压缩响应**（试过`--compressed`和`Accept-Encoding: identity`，服务端一律无视），需要TLS+gzip解压，工程量和固件体积顾虑一度让代理转发看起来更划算。

改为直连的原因：P5.0引入`cloud_transport`组件（TLS证书校验+SSE，为SiliconFlow/DeepSeek准备）后，HTTPS基础设施本来就要有，天气顺带复用不再是"额外成本"。gzip解压实测**零额外代码体积**——ESP32 ROM自带miniz的`tinfl_decompress`函数（`rom/miniz.h`），只需手动剥离10字节gzip容器头（RFC1952，含FEXTRA/FNAME/FCOMMENT可选字段处理）再喂给ROM函数，不需要vendor任何第三方代码。

**踩过的坑**：`tinfl_decompress_mem_to_mem()`便捷封装内部会把约11KB的`tinfl_decompressor`结构体（3张huffman表）放在调用者的栈上，`weather_task`默认4096字节栈直接被打爆导致`stack overflow`重启。改用底层`tinfl_decompress()`+`calloc`堆分配规避，`weather_task`栈同时提高到8192字节（TLS握手本身也有栈开销）。**任何调用ROM miniz解压函数的组件都要留意这个坑**，不要用`_mem_to_mem`系列便捷函数。

实测：真机确认`esp-x509-crt-bundle: Certificate validated`（TLS证书链校验通过），bridge日志确认重启后再无`/api/weather`请求。QWeather API Key现已进入`secrets.h`（`QWEATHER_API_KEY`/`QWEATHER_API_HOST`/`QWEATHER_LOCATION`），`bridge/`只保留P4 Claude用量代理（数据源`omp usage`命令本来就只能在Mac本地跑，没有直连替代方案）。`bridge/config.local.json`已删除（原来唯一内容就是QWeather配置）。

## P5.0 完成记录(云 API 与资源验证)

范围：扩展`cloud_transport`支持流式POST+取消；新增`llm_provider`(SiliconFlow/DeepSeek统一
接口，SSE解析+tool_calls增量归一化+429/503/504重试一次)、`asr_client`(multipart WAV上传)、
`tts_client`(PCM流式回调，跨网络分片字节对齐拼接)。暂不接麦克风，用固定文本+内置测试WAV验证。

**验收标准核对**(对照`docs/voice-agent-design.md` P5.0节)：
- ✅ 16k mono WAV能被ASR接受：真机上传内置测试WAV(`say`生成+`afconvert`转16kHz/mono/16-bit，
  canonical WAV无多余chunk)，SiliconFlow返回识别文本与原始语音内容一致
- ✅ SiliconFlow LLM普通SSE回答可解析；`llm_provider_init()`一行切provider kind即可切
  SiliconFlow/DeepSeek，不改业务代码——但**DeepSeek因API Key未提供，只做了编译验证，
  未做真实API调用**，待补Key后验证
- ✅ 用`get_device_status`只读工具完成两次LLM请求闭环：第1次模型发起工具调用(SiliconFlow)，
  本地执行(测试用固定假数据)，工具结果拼回历史发第2次请求，模型正确引用假数据生成自然语言
  回答("当前设备电量为75%。")——确认tool_calls增量格式(index/id/function.name/arguments
  跨chunk累积)已正确归一化
- ✅ TTS PCM声道/位宽/端序/chunked正确：接收样本数×2字节与预期播放时长吻合(2.8~2.9秒对应
  日常应答长度)；核心正确性点(网络分片不保证2字节对齐，`tts_client`内部1字节残留缓冲跨chunk
  拼接)在真实网络条件下验证无误
- ✅ 断网(不可达IP)、401、429、503、504、超时、取消(3秒后置位cancel_flag)均正确释放
  资源：用本地Python mock server(`http.server`标准库，明文HTTP，`cloud_transport`对
  `http://`会跳过TLS，不依赖真实API限流时机做可重复测试)逐一验证，堆/PSRAM在多轮ASR/LLM/
  TTS/错误路径测试后与启动完成时基本持平(无显著下降，`历史最低`水位是测试期间大缓冲区的
  瞬时占用,不是泄漏)
- ✅ TLS开启后固件体积可接受：见下方数据；ESP-SR尚未接入(P5.1工作)，其体积影响留给P5.1实测

**固件体积**(R0后基线 vs P5.0后)：

| 指标 | R0后 | P5.0后 | 差值 |
|---|---|---|---|
| 固件总镜像 | 1,116,179 字节 | 1,189,147 字节 | +72,968字节(+6.5%) |
| DIRAM占用率 | 37.96% | 38.06% | +0.1pp |
| 启动完成时堆剩余 | 约8.56MB | 约8.55MB | 基本持平 |
| 启动完成时PSRAM剩余 | 约8.38MB | 约8.38MB | 基本持平 |

**踩过的坑**：
1. **FATFS `rename()`不支持覆盖**：ESP-IDF的`esp_vfs_fat`基于ChaN FatFs，`f_rename()`严格
   要求目标文件不存在，不同于POSIX `rename()`的"覆盖"语义——`storage_sd_atomic_replace()`
   deepen于R0，但从未被真正调用过，这个bug潜伏到P5.0第一次真实调用才暴露：目标文件已存在时
   `rename()`返回`EEXIST`，函数返回`ESP_FAIL`。修复：`rename()`前先`remove()`旧文件(忽略
   `ENOENT`)。牺牲了"unlink和rename之间掉电会短暂缺文件"的极窄窗口，但保住了函数真正要防
   的问题(写到一半掉电导致内容损坏/截断)。
2. **DMA不能直接从flash映射(XIP)地址取数**：`EMBED_FILES`嵌入固件的测试WAV数据指针在flash，
   直接传给`storage_sd_atomic_replace()`——SDMMC写入path经DMA，`fwrite()`"成功"返回
   (`written==len`，无错误)，但SD卡上落盘内容全是0，不报错、极难排查。真机上靠对比
   embedded源字节(`52 49 46 46`="RIFF"，正确)和SD读回字节(`00 00 00 00`，全零)才定位到。
   **规则**：任何flash常量(`static const`大数组、`EMBED_FILES`资源)要写SD卡，必须先
   `memcpy`到堆/PSRAM缓冲区，`storage_sd.h`已补充文档说明。

DeepSeek API Key当前为空(用户尚未申请/填入)，`llm_provider`/`deepseek_client.c`已按与
SiliconFlow相同的OpenAI兼容协议完整实现并编译验证，`app_main.c`里`use_deepseek_llm`改
`true`即可切换——但encode真实DeepSeek API的行为(尤其SSE tool_calls分片格式是否与
SiliconFlow一致)尚未真机验证，待Key补齐后需要补一次真实调用验证。

## 已知风险 / 待确认事项

- ~~SD卡引脚~~ 已确认：SDMMC外设1-bit模式，CLK=38/CMD=21/D0=39，与显示SPI无冲突（详见`docs/hardware-pinmap.md`）
- ~~ST7305初始化序列~~ 已从官方`08_LVGL_V8_Test`摘取（登记于`docs/vendor-provenance.md`）
- **ST7305像素排布特殊**：横屏400×300时每字节打包2×4像素、交错位序，`index=(x/2)*75+(299-y)/4`，帧缓冲15000字节；不能按常规行连续1bpp处理，LVGL接入时flush回调需逐像素转换
- WiFi凭证/API Key等密钥一律不进仓库，开发阶段用`.gitignore`排除的本地头文件占位，正式方案后续再定
- ~~烧录卡在"No serial data received"~~ 已解决：原因是USB连接会话失效(设备节点`/dev/cu.usbmodem101`残留但无实际数据通路)，**重新插拔USB后设备换了新节点`/dev/cu.usbmodem1101`即可正常烧录**——纯软件侧(检查`ioreg`枚举、DTR/RTS复位脉冲)排查不出这类情况，本质是物理连接层，遇到同症状先重插USB
- **多任务并发读同一I2C设备会导致时序错乱**：`env_logger_task`(60s周期)与`ui_task`(page_home on_enter)同时调用`shtc3_read()`时，无锁的多步命令时序(唤醒→测量→读取→休眠)会被另一任务的命令打断，导致`ESP_ERR_INVALID_RESPONSE`。已修复：`sensor_shtc3`内部加`SemaphoreHandle_t`互斥锁串行化整个读取时序。**新增任何会被多任务调用的I2C多步时序组件都要照此加锁**，不能假设ESP-IDF I2C驱动的单次事务线程安全等于设备协议时序安全
- **ASCII点阵字体只收录`0x20~0x5A`**（空格到大写Z）：`st7305_draw_text`/`draw_utf8`里的ASCII部分超出这个范围(如`~`0x7E、小写字母)会画成空心框而不是报错，很容易漏看。已踩过一次：`page_env_monitor`用`~`表示数值范围，画出来是空心框，改成`-`(0x2D，在范围内)解决。**新增展示字符串前检查有没有用到`0x5A`以外的ASCII符号**
- **多个组件互相REQUIRES会形成循环依赖**：曾想让`app_runtime`直接`#include "secrets.h"`，但`secrets.h`物理上在`main/`目录，若给`app_runtime`加`REQUIRES main`就会和`main`已经`REQUIRES app_runtime`（app_main.c调用app_runtime_start()）形成循环。解法：改成`app_runtime_config_t`结构体，由`main`读`secrets.h`后组装、`app_runtime`只认结构体不认头文件——**下层Module需要上层专属配置时用参数传入，不要反向REQUIRES上层**
- **GCC `-Werror=format-truncation` 对小栈缓冲区的`snprintf`格式串很敏感**：即使实际值不可能超长(如`%d/2`里的`n`来自0~60的采样数)，编译器按`int`理论最大值(10位数)估算就可能报"输出可能被截断"并因`-Werror`导致编译失败。缓冲区按格式串理论最坏长度留够余量(实测`char msg[24]`不够，改`char msg[40]`才过)，不要死抠"看起来够用"的最小值
- **sys_state/服务层缓存的"数据不可用"不等于"清零数值"**：`weather_client`/`claude_quota_client`收到`available:false`响应时若把数值字段清零再推给`sys_state`，页面就没法区分"从未成功过"和"曾经成功、现在旧了"——正确做法是把上一次的缓存值原样透传给`sys_state_apply_*()`，只翻转`available`标志位，由页面自己根据`updated_at`是否为0判断走哪种空状态/旧数据提示

## 分阶段路线图

| Phase | 内容 | 完成标准 |
|---|---|---|
| P0 | 项目初始化 ✅已完成并真机验证 | 固件烧录成功、串口hello心跳正常；实测确认：16MB Flash(QIO@80MHz)、8MB Octal PSRAM、双核240MHz |
| P1 | 板级点亮 ✅已完成并真机验证 | I2C扫描4/4器件在线、ST7305屏幕刷新成功、KEY/BOOT按键正常；页面框架(ui_manager+page_diag)跑通 |
| P2 | 环境监测MVP：RTC+SHTC3+SD打点+趋势页面 ✅已完成并真机验证 | CSV持续写入(2个采样周期0错误)，SD卡格式化后持久挂载，断电重启RTC不丢时间 |
| P3 | 联网时钟天气：WiFi+NTP+mDNS+和风天气 ✅已完成并真机验证 | 显示正确本地时间与指定城市天气(bridge日志确认ESP32多次成功轮询`/api/weather`拿到200，`page_weather`展示"阴/东风0级/29℃/89%"等真实数据) |
| P4 | Claude额度显示：Mac桥接服务+ESP32轮询+额度页面 ✅已完成并真机验证 | 屏幕数字与手动核对吻合(bridge日志确认ESP32多次成功轮询`/api/quota/compact`拿到200) |
| R0 | 架构治理：`app_runtime`、`sys_state`、`input_manager`、深化 `storage_sd`、UI V2四页迁移 ✅已完成并真机验证 | 见上方"R0 完成记录"；固件+3,400字节(+0.3%)，quota/weather真机验证含bridge主动断开90s恢复场景 |
| P5.0 | 云 API 与资源验证：SiliconFlow ASR/TTS、SiliconFlow/DeepSeek LLM、SSE/Tools ✅已完成并真机验证(DeepSeek因API Key暂缺仅完成编译验证) | 见下方"P5.0 完成记录" |
| P5.1-P5.2 | 语音链路：音频、ESP-SR、Wake/VAD/ASR/LLM/TTS | 唤醒可靠，单轮问答闭环完整 |
| P6.0-P6.1 | 板载 agent：Pi 风格 loop、本地工具、SD 会话、Web/Skills | 工具权限和上下文预算可控；外部网页不能越权 |
| P6.2-P6.3 | 个人智能体：板载记忆、提醒/cron，按需接 HA 或 Telegram | 记忆可重建、可删除；掉电任务不重复执行 |
| P7-P8 | 可选 Companion 与产品化：仅重型能力、稳定性、OTA、安全 | Companion 断开不影响 L1-L4；长期运行稳定 |

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
