#!/usr/bin/env python3
"""usage bridge v3: 以 spool 仪表盘会话读取用量, 拼成固件 /v1/usage 契约供 ESP32 拉取.

数据源(全部为网站同源, 设备数字与网页一致):
  1. spool 仪表盘 /api/v1/usage/dashboard/models  -> 按模型当日统计(登录会话)
     会话: 邮箱密码自动登录并续期, 令牌轮换落盘
  2. 中转 /v1/usage (sk- key) -> 日/周额度与近7天逐日费用+token(套餐口径)

底部一行 = 古诗词名句轮换(随数据刷新换句, 内置字库内文案, 无需外部文件)。

仅用标准库; 后台线程定时刷新, 结果缓存供 HTTP 快速响应。
"""

import argparse
import json
import os
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TZ_CN = timezone(timedelta(hours=8))
SESSION_STATE = os.path.join(tempfile.gettempdir(), "usage_bridge_session.json")


def log(msg):
    """带时间戳的日志: 断链/恢复的时间点要能从事后日志里看出来。"""
    print(f"[{datetime.now():%m-%d %H:%M:%S}] {msg}")

# 底部轮换诗句(全部 ≤20 字, GB2312 字库内)
POEMS = (
    "海上生明月，天涯共此时",
    "海内存知己，天涯若比邻",
    "但愿人长久，千里共婵娟",
    "大漠孤烟直，长河落日圆",
    "采菊东篱下，悠然见南山",
    "落霞与孤鹜齐飞，秋水共长天一色",
    "春风又绿江南岸，明月何时照我还",
    "两情若是久长时，又岂在朝朝暮暮",
    "身无彩凤双飞翼，心有灵犀一点通",
    "抽刀断水水更流，举杯消愁愁更愁",
    "天生我材必有用，千金散尽还复来",
    "长风破浪会有时，直挂云帆济沧海",
    "会当凌绝顶，一览众山小",
    "欲穷千里目，更上一层楼",
    "不识庐山真面目，只缘身在此山中",
    "山重水复疑无路，柳暗花明又一村",
    "沉舟侧畔千帆过，病树前头万木春",
    "问君能有几多愁，恰似一江春水向东流",
    "衣带渐宽终不悔，为伊消得人憔悴",
    "蓦然回首，那人却在灯火阑珊处",
    "人生自古谁无死，留取丹心照汗青",
    "粉身碎骨浑不怕，要留清白在人间",
    "千磨万击还坚劲，任尔东西南北风",
    "纸上得来终觉浅，绝知此事要躬行",
    "问渠那得清如许，为有源头活水来",
    "等闲识得东风面，万紫千红总是春",
    "不畏浮云遮望眼，自缘身在最高层",
    "老骥伏枥，志在千里",
    "对酒当歌，人生几何",
    "月落乌啼霜满天，江枫渔火对愁眠",
    "春潮带雨晚来急，野渡无人舟自横",
    "东边日出西边雨，道是无晴却有晴",
    "曾经沧海难为水，除却巫山不是云",
    "天街小雨润如酥，草色遥看近却无",
    "停车坐爱枫林晚，霜叶红于二月花",
    "日出江花红胜火，春来江水绿如蓝",
    "春色满园关不住，一枝红杏出墙来",
    "小荷才露尖尖角，早有蜻蜓立上头",
    "黄河之水天上来，奔流到海不复回",
    "莫愁前路无知己，天下谁人不识君",
    "桃花潭水深千尺，不及汪伦送我情",
    "孤帆远影碧空尽，唯见长江天际流",
    "两岸猿声啼不住，轻舟已过万重山",
    "飞流直下三千尺，疑是银河落九天",
    "姑苏城外寒山寺，夜半钟声到客船",
    "近水楼台先得月，向阳花木易为春",
    "竹外桃花三两枝，春江水暖鸭先知",
    "欲把西湖比西子，淡妆浓抹总相宜",
    "水光潋滟晴方好，山色空蒙雨亦奇",
    "何当共剪西窗烛，却话巴山夜雨时",
    "春蚕到死丝方尽，蜡炬成灰泪始干",
    "谁言寸草心，报得三春晖",
    "露从今夜白，月是故乡明",
    "星垂平野阔，月涌大江流",
    "无边落木萧萧下，不尽长江滚滚来",
    "安得广厦千万间，大庇天下寒士俱欢颜",
    "会挽雕弓如满月，西北望，射天狼",
    "十年生死两茫茫，不思量，自难忘",
    "乱石穿空，惊涛拍岸，卷起千堆雪",
    "人生如梦，一尊还酹江月",
    "醉里挑灯看剑，梦回吹角连营",
    "了却君王天下事，赢得生前身后名",
    "众鸟高飞尽，孤云独去闲",
    "举头望明月，低头思故乡",
    "红豆生南国，春来发几枝",
    "独在异乡为异客，每逢佳节倍思亲",
    "劝君更尽一杯酒，西出阳关无故人",
    "夕阳无限好，只是近黄昏",
    "借问酒家何处有，牧童遥指杏花村",
    "清明时节雨纷纷，路上行人欲断魂",
)


def http_json(url, body=None, token=None, timeout=20):
    """POST/GET JSON; body=None 时为 GET。返回解析后的响应或抛出 OSError。"""
    headers = {"Content-Type": "application/json", "User-Agent": "usage-bridge/1.0"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    data = json.dumps(body).encode() if body is not None else None
    request = urllib.request.Request(url, data=data, method="POST" if body is not None else "GET",
                                     headers=headers)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(request, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def unwrap(resp):
    """兼容 {code,data} 包装与裸 JSON 两种返回。"""
    if isinstance(resp, dict) and "data" in resp and isinstance(resp["data"], (dict, list)):
        if "code" in resp and resp["code"] not in (0, 200, "0", 200):
            raise OSError(f"API code={resp['code']}: {resp.get('message')}")
        return resp["data"]
    return resp


class SpoolSession:
    """spool 仪表盘会话: 登录/续期/带鉴权 GET, 令牌持久化到本地状态文件。"""

    def __init__(self, base_url, email, password):
        self.base = base_url.rstrip("/") + "/api/v1"
        self.email = email
        self.password = password
        self.refresh_token = ""
        self.access = None
        self.expires_at = 0.0
        self._load_state()
        self.available = False

    def _load_state(self):
        try:
            state = json.load(open(SESSION_STATE, encoding="utf-8"))
            self.access = state.get("access_token")
            self.refresh_token = state.get("refresh_token") or ""
            self.expires_at = state.get("expires_at", 0.0)
        except (OSError, ValueError):
            pass

    def _save_state(self):
        try:
            json.dump({"access_token": self.access, "refresh_token": self.refresh_token,
                       "expires_at": self.expires_at}, open(SESSION_STATE, "w", encoding="utf-8"))
        except OSError as exc:
            print(f"[spool] 状态落盘失败: {exc}")

    def _unwrap_token(self, resp):
        data = unwrap(resp)
        if not isinstance(data, dict) or not data.get("access_token"):
            raise OSError(f"登录/续期响应缺少 access_token: {str(data)[:120]}")
        self.access = data["access_token"]
        self.refresh_token = data.get("refresh_token") or self.refresh_token
        self.expires_at = time.time() + float(data.get("expires_in") or 3600)
        self._save_state()
        return self.access

    def login(self):
        resp = http_json(f"{self.base}/auth/login",
                         {"email": self.email, "password": self.password})
        self._unwrap_token(resp)
        log("[spool] 密码登录成功")

    def refresh(self):
        if not self.refresh_token:
            raise OSError("无 refresh_token")
        resp = http_json(f"{self.base}/auth/refresh", {"refresh_token": self.refresh_token})
        self._unwrap_token(resp)
        log("[spool] refresh 续期成功")

    def ensure(self):
        """确保有可用 access token; 过期前 60s 内也续期。"""
        if self.access and time.time() < self.expires_at - 60:
            return
        try:
            self.refresh()
        except OSError:
            if self.email and self.password:
                self.login()
            else:
                raise

    def get(self, path, params=None):
        query = ""
        if params:
            query = "?" + urllib.parse.urlencode(params)
        try:
            self.ensure()
            return unwrap(http_json(f"{self.base}{path}{query}", token=self.access))
        except OSError as exc:
            if "401" not in str(exc) and "HTTP Error" not in str(exc):
                raise
            # 401: 续期后重试一次
            self.access = None
            self.ensure()
            return unwrap(http_json(f"{self.base}{path}{query}", token=self.access))


class RelayClient:
    """中转 /v1/usage (sk-): 日/周额度与近7天逐日数据(套餐口径)。"""

    def __init__(self, base_url, api_key, ttl):
        self.base_url = base_url.rstrip("/")
        self.api_key = api_key
        self.ttl = ttl
        self.data = None
        self.fetched_at = 0.0

    def fetch(self, force=False):
        if not self.api_key:
            return self.data
        if not force and self.data and time.time() - self.fetched_at < self.ttl:
            return self.data
        url = f"{self.base_url}/v1/usage?days=7&timezone=Asia/Shanghai"
        request = urllib.request.Request(url, headers={
            "Authorization": f"Bearer {self.api_key}",
            "User-Agent": "usage-bridge/1.0",  # rq 站拦 python-urllib 默认 UA
        })
        try:
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
            with opener.open(request, timeout=20) as resp:
                self.data = json.loads(resp.read().decode("utf-8"))
                self.fetched_at = time.time()
        except (OSError, ValueError) as exc:
            log(f"[relay] 拉取失败: {exc}")
        return self.data


class ModelAggregator24H:
    """过去 24 小时模型用量精确滑窗聚合器。

    从 spool /api/v1/usage 拉取明细，维护 24 小时内存滑窗；
    冷启动时向前拉取到 24h 截止线，之后每轮增量检查第 1 页并剔除过期记录。
    """

    def __init__(self, spool):
        self.spool = spool
        self.records = {}  # id -> (created_at_utc, item)
        self.models = []

    def update(self):
        cutoff = datetime.now(timezone.utc) - timedelta(hours=24)
        page = 1
        new_items = []
        keep_going = True
        while keep_going and page <= 20:
            resp = self.spool.get("/usage", {"page": page, "page_size": 100})
            items = resp.get("items", []) if isinstance(resp, dict) else []
            if not items:
                break
            for it in items:
                rec_id = it.get("id")
                ca_str = it.get("created_at")
                if not rec_id or not ca_str:
                    continue
                try:
                    ca = datetime.fromisoformat(ca_str)
                except ValueError:
                    continue
                if ca < cutoff:
                    keep_going = False
                    break
                if rec_id in self.records:
                    keep_going = False
                    break
                new_items.append((rec_id, ca, it))
            page += 1

        for rec_id, ca, it in new_items:
            self.records[rec_id] = (ca, it)

        # 剔除超过 24h 的过期记录
        expired = [rid for rid, (ca, _) in self.records.items() if ca < cutoff]
        for rid in expired:
            del self.records[rid]

        # 聚合各模型
        models_map = {}
        for rid, (ca, it) in self.records.items():
            m = it.get("model")
            if not m:
                continue
            if m not in models_map:
                models_map[m] = {
                    "model": m,
                    "requests": 0,
                    "input_tokens": 0,
                    "cache_read_tokens": 0,
                    "cache_creation_tokens": 0,
                    "output_tokens": 0,
                    "total_tokens": 0,
                    "cost": 0,
                }
            st = models_map[m]
            inp = it.get("input_tokens", 0) or 0
            out = it.get("output_tokens", 0) or 0
            cached = (it.get("cache_read_tokens", 0) or 0) + (it.get("cache_creation_tokens", 0) or 0)
            st["requests"] += 1
            st["input_tokens"] += inp
            st["output_tokens"] += out
            st["cache_read_tokens"] += cached
            st["total_tokens"] += it.get("total_tokens", inp + out + cached) or (inp + out + cached)

        models = list(models_map.values())
        models.sort(key=lambda x: x["total_tokens"], reverse=True)
        self.models = models
        return models


def build_payload(models, relay, poem):
    """按固件解析契约拼 JSON; token/模型=24小时滑窗模型, 今日token/额度/逐日=中转套餐, 底行=诗句。"""
    if relay and relay.get("usage") and relay["usage"].get("today"):
        relay_today = relay["usage"]["today"]
        today = {
            "input_tokens": relay_today.get("input_tokens", 0) or 0,
            "cache_read_tokens": relay_today.get("cache_read_tokens", 0) or 0,
            "cache_creation_tokens": relay_today.get("cache_creation_tokens", 0) or relay_today.get("cache_write_tokens", 0) or 0,
            "output_tokens": relay_today.get("output_tokens", 0) or 0,
            "requests": relay_today.get("requests", 0) or 0,
            "total_tokens": relay_today.get("total_tokens", 0) or 0,
            "cost": relay_today.get("cost", 0) or 0,
        }
    else:
        today = {"input_tokens": 0, "cache_read_tokens": 0, "cache_creation_tokens": 0,
                 "output_tokens": 0, "requests": 0, "total_tokens": 0, "cost": 0}
        for m in models:
            for key in ("input_tokens", "cache_read_tokens", "cache_creation_tokens",
                        "output_tokens", "requests", "total_tokens"):
                today[key] += m[key]

    payload = {
        "isValid": True,
        "usage": {"today": today},
        "model_stats": models,
        "daily_usage": (relay or {}).get("daily_usage") or [],
    }
    if relay and relay.get("subscription"):
        payload["subscription"] = relay["subscription"]
    if poem:
        payload["novel"] = {"text": poem}  # 沿用固件 novel 字段契约
    return payload


class Bridge:
    def __init__(self, args):
        self.spool = SpoolSession(args.relay_url, args.spool_email, args.spool_password)
        self.aggregator = ModelAggregator24H(self.spool)
        self.relay = RelayClient(args.relay_url, args.relay_key, args.remote_ttl)
        self.refresh = args.refresh
        self.lock = threading.Lock()
        self.payload_bytes = b""
        self.fail_streak = 0

    def refresh_once(self):
        relay = self.relay.fetch()
        models = self.aggregator.update()
        poem = POEMS[int(time.time() // 30) % len(POEMS)]
        payload = build_payload(models, relay, poem)
        data = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.spool.available = True
        with self.lock:
            self.payload_bytes = data
        return payload

    def _mark_stale(self):
        """连续拉取失败后把缓存 payload 标为失效, 设备据此显示 OLD 角标而非默默端旧数据。"""
        with self.lock:
            if not self.payload_bytes:
                return
            try:
                payload = json.loads(self.payload_bytes.decode("utf-8"))
                payload["isValid"] = False
                self.payload_bytes = json.dumps(payload, separators=(",", ":")).encode("utf-8")
            except ValueError:
                pass

    def loop(self):
        while True:
            try:
                self.refresh_once()
                self.fail_streak = 0
            except Exception as exc:  # 后台线程不允许退出
                self.fail_streak += 1
                log(f"[refresh] 失败(连续{self.fail_streak}次): {exc}")
                if self.fail_streak == 3:
                    self._mark_stale()
            time.sleep(self.refresh)

    def start(self):
        try:
            payload = self.refresh_once()
            log(f"[bridge] 首次聚合完成: 今日 {payload['usage']['today']['total_tokens']} tokens, "
                f"24H {len(payload['model_stats'])} 模型, 额度={'有' if 'subscription' in payload else '无'}")
        except Exception as exc:
            log(f"[bridge] 首次聚合失败(后台重试): {exc}")
        threading.Thread(target=self.loop, daemon=True).start()

    def snapshot(self):
        with self.lock:
            return self.payload_bytes


def read_secret(path, name):
    """从固件 secrets.h 读取 #define 字符串值(密钥/凭据不入库)。"""
    try:
        for line in open(path, encoding="utf-8"):
            if name in line and '"' in line:
                return line.split('"')[1]
    except OSError:
        pass
    return ""


class Handler(BaseHTTPRequestHandler):
    bridge = None

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in ("/v1/usage", "/usage.json"):
            body = self.bridge.snapshot()
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif path == "/healthz":
            self.send_response(200)
            self.send_header("Content-Length", "2")
            self.end_headers()
            self.wfile.write(b"ok")
        else:
            self.send_error(404)

    def log_message(self, fmt, *args):
        if args and "/v1/usage" in str(args[0]):
            return  # 设备轮询不刷日志
        print(f"[http] {self.address_string()} {fmt % args}")


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
    root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    secrets = os.path.join(root_dir, "firmware", "main", "secrets.h")
    parser = argparse.ArgumentParser(description="spool 仪表盘用量桥")
    parser.add_argument("--port", type=int, default=8789)
    parser.add_argument("--refresh", type=float, default=30, help="聚合刷新间隔(秒)")
    parser.add_argument("--remote-ttl", type=float, default=120, help="中转额度缓存秒数")
    parser.add_argument("--relay-url", default="https://rq.yunyc.work")
    parser.add_argument("--relay-key", default=read_secret(secrets, "CODEX_USAGE_API_KEY"))
    parser.add_argument("--spool-email", default=read_secret(secrets, "SPOOL_EMAIL"))
    parser.add_argument("--spool-password", default=read_secret(secrets, "SPOOL_PASSWORD"))
    args = parser.parse_args()

    if not (args.spool_password or os.path.exists(SESSION_STATE)):
        print("[bridge] 未配置 SPOOL_PASSWORD (firmware/main/secrets.h) 且无历史会话")
    bridge = Bridge(args)
    Handler.bridge = bridge
    bridge.start()
    server = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    log(f"[bridge] 监听 0.0.0.0:{args.port}, 刷新 {args.refresh}s, 诗句库 {len(POEMS)} 句")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
