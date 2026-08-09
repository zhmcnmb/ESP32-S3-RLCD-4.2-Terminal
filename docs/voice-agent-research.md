# 语音功能调研：同类项目做法与去留建议

> 调研日期：2026-08-09。一手来源优先（GitHub 源码/README/issues、官方文档/博客），关键结论附 URL。
> 背景三问：a) 充电时语音唤醒疑似失效；b) 常驻 MultiNet 推理耗电；c) 开放语音聊天的实用性存疑。
> 原始分项报告已合并于此（技术方案 + 产品定位两部分）。

## 结论速览

1. **语音保留，但换定位**：从"开放聊天"改为"命令式入口 + 到点 TTS 播报"。开放聊天降级为 BOOT 短按显式发起的彩蛋，不作为卖点。
2. **MultiNet 常驻当唤醒网关是反官方用法**（官方要求 MultiNet 必须配合 WakeNet，CPU 代价约 4 倍）；没有"贾维斯"WakeNet 模型。可执行的中间方案：AFE VAD 门控 MultiNet，只在有人声时推理。
3. **充电唤不醒在所有同类仓库 issue 中无一手报告**；最接近的是小智官方 FAQ 把"充放电电源"列为 EMI 源。需自测取证（充电时录 AFE 输出回听），大概率是板级充电电路 EMI。
4. **ESP32 常驻唤醒做不到个位数 mA**（小智作者原话），HA 语音卫星全部按常供电设计——本项目"电池时降级为按键 PTT"的方向与业界一致。

## 第 1 部分：技术方案调研

### 1.1 78/xiaozhi-esp32（虾哥小智）

- **唤醒双路径**：有 MultiNet 模型就走 MultiNet（自定义唤醒词），否则走 WakeNet（默认"你好小智"）。AFE 用 `AFE_TYPE_VC + AFE_MODE_HIGH_PERF`，`ns_init=false`（降噪交后端）；唤醒成功后动态 `disable_wakenet()`，对话结束再 enable。无任何唤醒模型时 AFE 完全不启用，退化为纯按键设备（"Initialized as raw engine"）。
  来源：[afe_audio_engine.cc](https://github.com/78/xiaozhi-esp32/blob/main/main/audio/engines/afe_audio_engine.cc)
- **按键是一等公民**：按键唤醒与语音唤醒并存（[#1011](https://github.com/78/xiaozhi-esp32/issues/1011)）；官方量产小智卡明确"按钮、触控、语音三种唤醒方式"（[官方 PDF](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/static/pdf/static/zh_CN/core/Xiaozhi_Card_Kit.pdf)）。
- **低功耗**：作者原话"有专门的低功耗唤醒芯片，肯定是个位数 mA"，即 ESP32 常驻监听做不到（[#453](https://github.com/78/xiaozhi-esp32/issues/453)）；电池板靠大电池换续航，300mAh 约 4-6 小时（[#464](https://github.com/78/xiaozhi-esp32/issues/464)）。WiFi modem sleep 默认启用（[wifi_board.h](https://github.com/78/xiaozhi-esp32/blob/main/main/boards/common/wifi_board.h)）。
- **充电/噪声**：issue 区无"充电导致唤醒失效"确诊报告。最接近：#306 环境噪声大时唤醒困难，作者排查建议是"去后台听唤醒录音"（[#306](https://github.com/78/xiaozhi-esp32/issues/306)）；官方 FAQ Q7 把"充放电电源"列为应隔离的 EMI 源（[FAQ](https://xiaozhi.dev/docs/usage/faq/)）。
- **对话形态**：Opus 音频流 + WebSocket，listen 三态 `auto/manual/realtime`，auto 即连续对话、manual 即按键单轮（[websocket_zh.md](https://github.com/78/xiaozhi-esp32/blob/main/docs/websocket_zh.md)）。

### 1.2 esp-skainet + esp-sr 官方文档

- **官方定位**：WakeNet 是为常驻监听设计的低功耗指纹模型（"built upon neural network for low-power embedded MCUs"，[wake_word_engine](https://github.com/espressif/esp-sr/blob/master/docs/en/wake_word_engine/README.rst)）；MultiNet 官方原话"**must be used with WakeNet. After wake-up, MultiNet detection can start**"（[speech_command_recognition](https://github.com/espressif/esp-sr/blob/master/docs/en/speech_command_recognition/README.rst)）。本项目用 MultiNet 常驻当唤醒网关，本身是反官方推荐用法（无奈之举：WakeNet 无"贾维斯"模型，定制需走乐鑫商务流程）。
- **CPU 代价**：S3 基准 WakeNet9 每帧 3.0ms/32ms（约 9% 单核，误唤醒 12h 一次）；MultiNet5 Q8 12ms/32ms（约 37.5% 单核）——常驻 MultiNet 约 4 倍代价（[benchmark](https://github.com/espressif/esp-sr/blob/master/docs/en/benchmark/README.rst)）。
- **AFE 模式**：枚举只有 `AFE_MODE_LOW_COST / AFE_MODE_HIGH_PERF`；LOW_COST 是省算力变体而非电源管理（[esp_afe_config.h](https://github.com/espressif/esp-sr/blob/master/include/esp32s3/esp_afe_config.h)）。单麦只要 WakeNet 时官方推荐绕开 AFE 用裸接口（[skainet 例程](https://github.com/espressif/esp-skainet/blob/master/examples/wake_word_detection/README.md)）。
- esp-sr/esp-skainet issue 区同样无"充电导致唤醒失效"报告。

### 1.3 ESPHome / Home Assistant 语音卫星

- 端侧唤醒用 **microWakeWord**（TFLM 两级：micro_speech 预处理自带噪声抑制+AGC，流式模型多窗口平滑判决），预置 hey jarvis / okay nabu / alexa（[kahrendt/microWakeWord](https://github.com/kahrendt/microWakeWord)）；`stop_after_detection` 默认检测到即停推理（[ESPHome 文档](https://esphome.io/components/micro_wake_word.html)）。
- **这类卫星按常供电设计**（Voice PE 用 USB-C 5V/2A 无电池）；为省电做端侧唤醒的是 Android 手机（"preserves battery life"，[about_wake_word](https://www.home-assistant.io/voice_control/about_wake_word/)）。音频前端用专用 XMOS XU316 而不是让 ESP32-S3 自己算（[Voice PE](https://www.home-assistant.io/voice-pe/)）。
- **高频用途官方定位**：计时器、购物清单、设备控制、天气/温度问答、播报；LLM 开放对话明确标注"experimental"（[Chapter 5](https://www.home-assistant.io/blog/2023/12/13/year-of-the-voice-chapter-5/)）。

### 1.4 ESP-BOX 官方例程

官方链路 **AFE（双麦+AEC）→ WakeNet → MultiNet**，支持连续识别、唤醒打断、200+ 命令词（[esp-box](https://github.com/espressif/esp-box)）。再次印证：MultiNet 在官方体系里永远是"唤醒之后才跑"的第二级。

## 第 2 部分：产品定位调研

### 2.1 零语音设备靠什么留住用户

- **TRMNL**（e-ink 信息屏）：卖点"不打扰的一瞥"与单向轮询隐私；日历/天气/待办是 pull 式常显信息流。用户口碑高频词 calendar/weather/glanceable（[trmnl.com](https://trmnl.com/)）。
- **Tidbyt / AWTRIX**（像素钟）：留存靠自选信息轮播 + HA/MQTT 通知末端（AWTRIX 还有 RTTTL 提示音）。公司被收编后社区用开源后端 Tronbyt 续命——粘性在"这块屏显示我的数据"本身（[macwright.com](https://macwright.com/2025/04/12/tidbyt-second-life)、[awtrix3](https://github.com/blueforcer/awtrix3)）。

### 2.2 有语音设备的真实用途

- **Atom Echo 类（HA 语音卫星）**：三年社区压倒性真实用途是命令式控制（"Pick up the tiny device to talk to your smart home. Issue commands and get responses!"，[HA 官方教程](https://www.home-assistant.io/voice_control/thirteen-usd-voice-remote/)）；几乎无人用于开放聊天。喇叭弱、唤醒词稳定性是长期痛点，但不妨碍"命令入口"持续被用。
- **小智 AI**：传播靠情感陪伴叙事（儿童/老人，[腾讯新闻](https://news.qq.com/rain/a/20251107A06KR300)）；但二开社区最高频加的功能是音乐、闹钟提醒、倒计时、天气、对讲、远程控制（[Gitee 二开固件](https://gitee.com/wt512/xiaozhi-plus-firmware)）——清一色命令式/工具式。
- **ElectronBot / Emo / Loona**：陪伴型留存权重在表情/动作/人格三件套，语音只是触发器；原版 ElectronBot 甚至无语音（[peng-zhihui/ElectronBot](https://github.com/peng-zhihui/ElectronBot)）。开放聊天能成立全部依赖人格化路线。

### 2.3 三种语音形态评估

| 形态 | 判定 | 依据 |
|---|---|---|
| a) 开放式聊天 | 新鲜感型价值，除非走人格化陪伴路线 | 小喇叭音质限制上限；长期留存案例全部依赖人格/表情/记忆三件套 |
| b) 命令式语音 | **被反复验证的长期价值** | HA 卫星三年长青；小智二开 Top 需求（闹钟/倒计时/天气/控制）；响应短可模板化，小喇叭够用 |
| c) 纯 TTS 主动播报 | **被低估的差异化** | TRMNL 类做不到出声，Echo 类没有屏；"到点播报+屏上同步"是两类产品中间的空白 |

优先级：c ≈ b > a。且 c/b 共用本项目已有的 ASR/LLM/TTS 管线与 scheduler，边际成本低。

## 第 3 部分：对本项目的建议

1. **语音不删，叙事换掉**：定位改为"命令入口 + 到点播报"。一等公民意图收敛为：定提醒/闹钟、倒计时、问天气、问温湿度、问时间——恰好是已有的 scheduler/weather/env 能力，可做模板化快速路径（固定意图跳过 LLM 往返），把"一句话办成事"的延迟和可靠性做到位。
2. **开放聊天降级为 PTT 彩蛋**：BOOT 短按显式发起，不追求常驻免唤醒闲聊；不投入 persona/长记忆/表情系统——那是路线变更，不是功能微调。
3. **加深 TTS 播报差异化**：提醒播报继续投入（播报时屏幕联动显示内容、重复提醒、静音时段），这是 TRMNL 类做不了、Echo 类没屏的组合位置。
4. **常驻推理再降一档：VAD 门控 MultiNet**。只在 `vad_state==SPEECH` 的语音段调 `multinet->detect()`，静音段跳过——常驻 CPU 从 ~37.5% 单核压到有人声时才支出，误唤醒随推理时长同比下降；不需要新模型，只动 `wakeword_loop` 调度。USB/电池两种供电都受益。电池模式可再激进：停 AFE、I2S 挂起，PTT 时再拉起。
5. **充电唤不醒先取证再修**：用 esp-sr 的 `afe_debug_hook_callback` 在充电状态录一段 AFE 输出落 SD 卡回听（对标小智 #306 的排查法），确认是充电 EMI 淹没语音还是检测退化；同时可换 5V2A 充电头对比。若确认稳态开关噪声，优先考虑开 AFE NS 或 feed 前高通/陷波，不动检测逻辑。
6. **把克制写成卖点**：TRMNL 卖"单向轮询"，HA 卖"本地优先"。本项目"唤醒本地推理、云端仅 ASR/LLM/TTS、电池自动降级 PTT"已是同类最克制方案，应写进 README 定位。

## 附：调研局限与来源

> 局限：小智"留存"无官方口径，用二开社区诉求与出圈事件交叉推断；TRMNL/Tidbyt 场景来自官方评测区（偏正向）；"充电唤醒失效"在各仓库 issue 中均无一手报告，本项目的失效需按建议 5 自测取证。

- 技术：[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 及其 [#306](https://github.com/78/xiaozhi-esp32/issues/306) [#453](https://github.com/78/xiaozhi-esp32/issues/453) [#464](https://github.com/78/xiaozhi-esp32/issues/464) [#1011](https://github.com/78/xiaozhi-esp32/issues/1011) · [小智 FAQ](https://xiaozhi.dev/docs/usage/faq/) · [esp-sr WakeNet](https://github.com/espressif/esp-sr/blob/master/docs/en/wake_word_engine/README.rst) · [esp-sr MultiNet](https://github.com/espressif/esp-sr/blob/master/docs/en/speech_command_recognition/README.rst) · [esp-sr benchmark](https://github.com/espressif/esp-sr/blob/master/docs/en/benchmark/README.rst) · [esp-skainet](https://github.com/espressif/esp-skainet) · [microWakeWord](https://github.com/kahrendt/microWakeWord) · [ESPHome micro_wake_word](https://esphome.io/components/micro_wake_word.html) · [HA wake word 架构](https://www.home-assistant.io/voice_control/about_wake_word/) · [HA Voice PE](https://www.home-assistant.io/voice-pe/) · [esp-box](https://github.com/espressif/esp-box)
- 产品：[trmnl.com](https://trmnl.com/) · [github.com/usetrmnl](https://github.com/usetrmnl) · [tidbyt.com](https://tidbyt.com/products/tidbyt) · [macwright.com](https://macwright.com/2025/04/12/tidbyt-second-life) · [awtrix3](https://github.com/blueforcer/awtrix3) · [ElectronBot](https://github.com/peng-zhihui/ElectronBot) · [KEYi 对比](https://keyirobot.com/blogs/buying-guide/loona-vs-vector-vs-emo-which-robot-pet-is-right-for-you) · [HA $13 教程](https://www.home-assistant.io/voice_control/thirteen-usd-voice-remote/) · [HA Year of the Voice Ch.5](https://www.home-assistant.io/blog/2023/12/13/year-of-the-voice-chapter-5/) · [Gitee 小智二开](https://gitee.com/wt512/xiaozhi-plus-firmware)
