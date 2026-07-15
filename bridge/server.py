#!/usr/bin/env python3
"""Claude用量桥接服务（P4）。

跑在Mac本地，供ESP32局域网轮询Claude订阅用量(5小时/7天窗口)。

## 用量数据源
`omp usage --provider anthropic --json`——命中Anthropic官方(未文档化)OAuth用量
接口，账号级权威数据，和Claude Code自带/usage背后是同一个接口。详见
docs/CLAUDE.md "P4 用量数据源" 一节的调研结论。


纯Python标准库实现，零pip依赖。

用法:
    python3 bridge/server.py [--port 8787] [--refresh 60]

前置依赖:
  - Claude用量: 运行本脚本的Mac上 `omp` 必须在PATH且已登录Anthropic
失败时对应接口返回 available:false 降级。
"""

import argparse
import json
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


# 缓存: 全局单份、读写都加锁。不做增量/持久化，进程重启即重新拉取。
_lock = threading.Lock()
_quota_cache = {"available": False, "error": None, "fetched_at": 0, "5h": None, "7d": None}


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


        self._json({"error": "not found"}, status=404)

    def log_message(self, fmt, *args):
        sys.stderr.write(f"[bridge] {self.address_string()} {fmt % args}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8787)
    parser.add_argument("--refresh", type=int, default=60, help="Claude用量刷新间隔(秒)")
    args = parser.parse_args()

    threading.Thread(target=refresh_loop, args=(fetch_usage, args.refresh), daemon=True).start()

    server = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print(f"[bridge] 监听 0.0.0.0:{args.port}，用量刷新{args.refresh}s")
    print("[bridge] mDNS主机名发现由macOS系统级Bonjour(mDNSResponder)自动广播，本进程无需额外处理")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
