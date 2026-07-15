# Mac 本地桥接服务（P4 Claude用量）

跑在Mac本地的轻量HTTP服务，供ESP32局域网轮询Claude订阅用量。天气已改为ESP32设备通过HTTPS直连QWeather（TLS证书链+ROM miniz解gzip现已在设备侧实现），不再需要Mac中转。

## 运行

```bash
python3 bridge/server.py [--port 8787] [--refresh 60]
```

纯Python标准库实现，零pip依赖，不需要虚拟环境。

**持久运行**：当前是前台脚本，进程退出服务就没了。长期使用建议用 `launchd`（macOS原生，开机自启+崩溃自动重启）配一个 plist，暂未配置——先按需手动跑。

## 前置依赖

| 功能 | 依赖 | 缺失时的行为 |
|---|---|---|
| Claude用量 | `omp` 在PATH且已登录Anthropic(`omp usage`能正常输出) | `/api/quota`返回`status:"unavailable"`降级 |

## Claude用量数据源

开发者本机用 **OMP框架**(`omp` CLI)使用 Claude 订阅，而不是官方 `claude` CLI，因此最初"解析 `~/.claude/projects/**/*.jsonl` 做近似估算"的方案已废弃，改为直接吃 OMP 自己的用量查询能力：`omp usage --provider anthropic --json`。这条命令直接命中 Anthropic 官方(未文档化)OAuth用量接口——和 Claude Code 自带 `/usage`、状态栏用量指示背后是**同一个接口**，返回账号级权威数据，不区分调用它的客户端。`fetch_usage()` 每 `--refresh` 秒(默认60s)调一次，解析出 `anthropic:5h`/`anthropic:7d` 两个窗口的用量+reset时间，缓存在内存供HTTP接口读取。

## 接口

- `GET /api/health` — `{"status": "ok"}`
- `GET /api/quota` — Claude用量完整信息，含错误详情：
  ```json
  {"status": "ok", "error": null, "fetched_at": 1784033643,
   "5h": {"used": 0.07, "resets_at": 1784050800},
   "7d": {"used": 0.27, "resets_at": 1784538000}}
  ```
- `GET /api/quota/compact` — ESP32专用精简版(去掉error字段)

隐私边界：Claude用量只返回聚合百分比与reset时间戳，绝不返回消息正文、账号邮箱、accountId等字段(`omp usage --json`原始响应里有这些，已显式过滤)；仅绑定局域网(0.0.0.0，不做任何公网转发/内网穿透)。

## ESP32端发现方式

不需要`server.py`自己做mDNS广播——macOS系统级Bonjour(`mDNSResponder`)本来就会自动广播本机`.local`主机名，任何监听端口的进程都能通过它被发现。ESP32侧(`firmware/components/claude_quota_client/`)用ESP-IDF的`mdns_query_a()`解析`secrets.h`里配置的`BRIDGE_MDNS_HOST`(如`sunjindeMacBook-Air`，不含`.local`后缀)拿到IP，再发HTTP GET。桥接服务不可达时ESP32端降级显示对应提示，下个周期自动重试，不阻断UI。
