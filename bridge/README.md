# Claude Code 用量桥接服务（Phase 4，尚未实现）

跑在Mac本地的轻量HTTP服务，供ESP32局域网轮询，展示Claude Code用量额度。ESP32无法直接访问Mac文件系统，因此需要这个桥接。

## 设计要点（详见项目规划记录，实现时按此展开）

- 纯Python标准库（`ThreadingHTTPServer`），零pip依赖，不引入Flask/FastAPI
- 数据源：`~/.claude/projects/**/*.jsonl`，增量解析(记录每文件offset)+定期后台重扫，避免ESP32轮询等待
- 两类指标：
  1. 账号级精确用量聚合（今日/近7天/按模型，token计数）
  2. Pro/Max 5小时会话滚动窗口的**近似**估算（官方未公开精确算法，需在响应中明确标注`approx:true`）
- 对外暴露 `/api/quota`（完整）、`/api/quota/compact`（ESP32专用精简JSON）、`/api/health`
- 隐私边界：只返回聚合计数，绝不返回消息正文；仅绑定局域网
- 发现方式：ESP32通过mDNS解析Mac主机名，组播失败时回退NVS静态IP

Phase 0阶段本目录仅为占位，具体代码等到Phase 4再实现。
