#!/usr/bin/env python3
"""Claude用量 + 天气 桥接服务（P4 + P3）。

跑在Mac本地，供ESP32局域网轮询：
  - Claude订阅用量(5小时/7天窗口)，数据源见下文
  - 和风天气(QWeather)当前实况，代理转发+gzip解压

## 用量数据源
`omp usage --provider anthropic --json`——命中Anthropic官方(未文档化)OAuth用量
接口，账号级权威数据，和Claude Code自带/usage背后是同一个接口。详见
docs/CLAUDE.md "P4 用量数据源" 一节的调研结论。

## 天气为什么要代理，不让ESP32直连QWeather
QWeather新版API Host(如 xxxxx.re.qweatherapi.com)不管Accept-Encoding怎么设都
强制gzip压缩响应，ESP-IDF的esp_http_client不做自动gzip解压，ESP32上手写gzip
解压要接ROM miniz的tinfl+手剥gzip头，工程量不小还要为此引入mbedtls整套HTTPS/
证书链。Python标准库`gzip.decompress()`一行代码解决，所以天气也走本桥接代理：
ESP32只需局域网明文HTTP，复用Claude用量已验证过的mDNS发现机制。

纯Python标准库实现，零pip依赖。

用法:
    python3 bridge/server.py [--port 8787] [--refresh 60] [--weather-refresh 900]

前置依赖:
  - Claude用量: 运行本脚本的Mac上 `omp` 必须在PATH且已登录Anthropic
  - 天气: bridge/config.local.json 需配置 qweather_api_key/api_host/location
    (该文件已在.gitignore里，不进仓库；配置模板见 config.local.json.example)
均缺失/失败时对应接口返回 available:false 降级，不影响另一个接口。
"""

import argparse
import gzip
import json
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

CONFIG_PATH = Path(__file__).resolve().parent / "config.local.json"

# 缓存: 全局单份、读写都加锁。不做增量/持久化，进程重启即重新拉取。
_lock = threading.Lock()
_quota_cache = {"available": False, "error": None, "fetched_at": 0, "5h": None, "7d": None}
_weather_cache = {"available": False, "error": None, "fetched_at": 0,
                   "temp": None, "humidity": None, "condition": None,
                   "wind_dir": None, "wind_scale": None}


def load_config():
    """读取 bridge/config.local.json，缺失/损坏时返回空dict(天气接口相应降级)"""
    try:
        return json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    except Exception:
        return {}


def fetch_usage():
    """调用 omp usage 拉取Anthropic用量，更新 _quota_cache。任何失败都降级为不可用，不抛异常"""
    try:
        proc = subprocess.run(
            ["omp", "usage", "--provider", "anthropic", "--json"],
            capture_output=True, text=True, timeout=15,
        )
        if proc.returncode != 0:
            raise RuntimeError(f"omp usage 退出码{proc.returncode}: {proc.stderr[:200]}")

        data = json.loads(proc.stdout)
        reports = data.get("reports", [])
        anthropic = next((r for r in reports if r.get("provider") == "anthropic"), None)
        if not anthropic:
            raise RuntimeError("响应中没有anthropic provider(未登录或未认证)")

        limits = {item["id"]: item for item in anthropic.get("limits", [])}
        h5 = limits.get("anthropic:5h")
        d7 = limits.get("anthropic:7d")
        if not h5 or not d7:
            raise RuntimeError("响应缺少anthropic:5h或anthropic:7d窗口")

        with _lock:
            _quota_cache.update({
                "available": True, "error": None, "fetched_at": int(time.time()),
                "5h": {"used": h5["amount"]["usedFraction"], "resets_at": h5["window"]["resetsAt"] // 1000},
                "7d": {"used": d7["amount"]["usedFraction"], "resets_at": d7["window"]["resetsAt"] // 1000},
            })
    except Exception as e:  # noqa: BLE001 - 尽力而为，任何失败都只降级不崩溃
        with _lock:
            _quota_cache["available"] = False
            _quota_cache["error"] = str(e)


def fetch_weather():
    """请求QWeather /v7/weather/now，自动处理gzip，更新 _weather_cache。任何失败都降级不抛异常"""
    cfg = load_config()
    key = cfg.get("qweather_api_key")
    host = cfg.get("qweather_api_host")
    location = cfg.get("qweather_location")
    if not (key and host and location):
        with _lock:
            _weather_cache["available"] = False
            _weather_cache["error"] = "未配置(bridge/config.local.json缺qweather_*字段)"
        return

    url = f"https://{host}/v7/weather/now?location={location}"
    try:
        req = urllib.request.Request(url, headers={"X-QW-Api-Key": key})
        with urllib.request.urlopen(req, timeout=10) as resp:
            raw = resp.read()
            if resp.headers.get("Content-Encoding") == "gzip":
                raw = gzip.decompress(raw)
            data = json.loads(raw)

        if data.get("code") != "200":
            raise RuntimeError(f"QWeather返回code={data.get('code')}")

        now = data["now"]
        with _lock:
            _weather_cache.update({
                "available": True, "error": None, "fetched_at": int(time.time()),
                "temp": float(now["temp"]), "humidity": int(now["humidity"]),
                "condition": now["text"], "wind_dir": now["windDir"], "wind_scale": now["windScale"],
            })
    except (urllib.error.URLError, KeyError, ValueError, RuntimeError) as e:
        with _lock:
            _weather_cache["available"] = False
            _weather_cache["error"] = str(e)


def refresh_loop(fn, interval_s):
    """后台线程: 启动时立即抓一次，之后按interval周期刷新"""
    while True:
        fn()
        time.sleep(interval_s)


def snapshot(cache):
    with _lock:
        return dict(cache)


class Handler(BaseHTTPRequestHandler):
    def _json(self, obj, status=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/api/health":
            self._json({"status": "ok"})
            return

        if self.path == "/api/quota":
            snap = snapshot(_quota_cache)
            # 隐私边界: 只返回聚合百分比+reset时间，不透出账号邮箱/accountId/endpoint等字段
            self._json({
                "status": "ok" if snap["available"] else "unavailable",
                "error": snap["error"], "fetched_at": snap["fetched_at"],
                "5h": snap["5h"], "7d": snap["7d"],
            })
            return

        if self.path == "/api/quota/compact":
            snap = snapshot(_quota_cache)
            self._json({
                "available": snap["available"], "fetched_at": snap["fetched_at"],
                "5h": snap["5h"], "7d": snap["7d"],
            })
            return

        if self.path == "/api/weather":
            snap = snapshot(_weather_cache)
            self._json({
                "available": snap["available"], "fetched_at": snap["fetched_at"],
                "temp": snap["temp"], "humidity": snap["humidity"],
                "condition": snap["condition"],
                "wind_dir": snap["wind_dir"], "wind_scale": snap["wind_scale"],
            })
            return

        self._json({"error": "not found"}, status=404)

    def log_message(self, fmt, *args):
        sys.stderr.write(f"[bridge] {self.address_string()} {fmt % args}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8787)
    parser.add_argument("--refresh", type=int, default=60, help="Claude用量刷新间隔(秒)")
    parser.add_argument("--weather-refresh", type=int, default=900, help="天气刷新间隔(秒)，默认15分钟")
    args = parser.parse_args()

    threading.Thread(target=refresh_loop, args=(fetch_usage, args.refresh), daemon=True).start()
    threading.Thread(target=refresh_loop, args=(fetch_weather, args.weather_refresh), daemon=True).start()

    server = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print(f"[bridge] 监听 0.0.0.0:{args.port}，用量刷新{args.refresh}s，天气刷新{args.weather_refresh}s")
    print("[bridge] mDNS主机名发现由macOS系统级Bonjour(mDNSResponder)自动广播，本进程无需额外处理")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
