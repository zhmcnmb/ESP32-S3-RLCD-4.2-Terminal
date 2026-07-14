# Mac 本地桥接服务（P3 天气 + P4 Claude用量，均已实现）

跑在Mac本地的轻量HTTP服务，供ESP32局域网轮询，代理两类数据：Claude订阅用量、和风天气实况。ESP32无法直接访问Mac子进程，也没法在设备上解决QWeather强制gzip压缩的问题，因此都走这一个桥接服务。

## 运行

```bash
python3 bridge/server.py [--port 8787] [--refresh 60] [--weather-refresh 900]
```

纯Python标准库实现，零pip依赖，不需要虚拟环境。

**持久运行**：当前是前台脚本，进程退出服务就没了。长期使用建议用 `launchd`（macOS原生，开机自启+崩溃自动重启）配一个 plist，暂未配置——先按需手动跑。

## 前置依赖

| 功能 | 依赖 | 缺失时的行为 |
|---|---|---|
| Claude用量 | `omp` 在PATH且已登录Anthropic(`omp usage`能正常输出) | `/api/quota`返回`status:"unavailable"`降级 |
| 天气 | `bridge/config.local.json` 配好 `qweather_api_key`/`qweather_api_host`/`qweather_location` | `/api/weather`返回`available:false`降级 |

两个功能互相独立，一个缺失不影响另一个。天气配置模板见 `config.local.json.example`：

```bash
cp bridge/config.local.json.example bridge/config.local.json
# 编辑填入真实值，该文件已在 .gitignore 里
```

## Claude用量数据源

开发者本机用 **OMP框架**(`omp` CLI)使用 Claude 订阅，而不是官方 `claude` CLI，因此最初"解析 `~/.claude/projects/**/*.jsonl` 做近似估算"的方案已废弃，改为直接吃 OMP 自己的用量查询能力：`omp usage --provider anthropic --json`。这条命令直接命中 Anthropic 官方(未文档化)OAuth用量接口——和 Claude Code 自带 `/usage`、状态栏用量指示背后是**同一个接口**，返回账号级权威数据，不区分调用它的客户端。`fetch_usage()` 每 `--refresh` 秒(默认60s)调一次，解析出 `anthropic:5h`/`anthropic:7d` 两个窗口的用量+reset时间，缓存在内存供HTTP接口读取。

## 天气为什么要代理，不让ESP32直连QWeather

实测发现：QWeather新版API Host(项目专属域名，如 `xxxxx.re.qweatherapi.com`)**不管请求方Accept-Encoding怎么设都强制gzip压缩响应**(试过`--compressed`和`Accept-Encoding: identity`，服务端一律无视，照样gzip)。ESP-IDF的`esp_http_client`不做自动gzip解压——要在ESP32上解，得接ROM里阉割版的miniz `tinfl`函数、手动剥离gzip头再喂给inflate，工程量不小，还得为HTTPS额外拉入`mbedtls`+`esp_crt_bundle`整套证书链，显著增大固件体积和维护面。

Python标准库 `gzip.decompress()` 一行代码解决。所以天气也走本桥接代理：`fetch_weather()` 每 `--weather-refresh` 秒(默认900s=15分钟，天气变化慢没必要更勤)请求一次QWeather，自动解压+解析，ESP32端`weather_client`复用Claude用量已验证过的mDNS发现机制，走局域网**明文HTTP**，完全不需要碰HTTPS/TLS/gzip。这也意味着和风天气的API Key**不会出现在烧录进设备的固件里**——只留在Mac本地的`config.local.json`，比之前直连方案的密钥暴露面更小。

## 接口

- `GET /api/health` — `{"status": "ok"}`
- `GET /api/quota` — Claude用量完整信息，含错误详情：
  ```json
  {"status": "ok", "error": null, "fetched_at": 1784033643,
   "5h": {"used": 0.07, "resets_at": 1784050800},
   "7d": {"used": 0.27, "resets_at": 1784538000}}
  ```
- `GET /api/quota/compact` — ESP32专用精简版(去掉error字段)
- `GET /api/weather` — 天气实况：
  ```json
  {"available": true, "fetched_at": 1784040273,
   "temp": 29.0, "humidity": 89, "condition": "阴",
   "wind_dir": "东风", "wind_scale": "0"}
  ```

隐私边界：Claude用量只返回聚合百分比与reset时间戳，绝不返回消息正文、账号邮箱、accountId等字段(`omp usage --json`原始响应里有这些，已显式过滤)；仅绑定局域网(0.0.0.0，不做任何公网转发/内网穿透)。

## ESP32端发现方式

不需要`server.py`自己做mDNS广播——macOS系统级Bonjour(`mDNSResponder`)本来就会自动广播本机`.local`主机名，任何监听端口的进程都能通过它被发现。ESP32侧(`firmware/components/claude_quota_client/`、`firmware/components/weather_client/`)都用ESP-IDF的`mdns_query_a()`解析`secrets.h`里配置的`BRIDGE_MDNS_HOST`(如`sunjindeMacBook-Air`，不含`.local`后缀)拿到IP，再发HTTP GET，两个客户端共用同一个桥接服务地址、各自独立轮询任务。桥接服务不可达时ESP32端降级显示对应提示，下个周期自动重试，不阻断UI。
