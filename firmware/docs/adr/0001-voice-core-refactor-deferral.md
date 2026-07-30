# ADR-0001: 暂缓语音链核心重构（turn_ctx_t / speak facade）

- 日期：2026-07-30
- 状态：Accepted（暂缓）
- 关联：architecture-review 报告候选 2、5

## 背景

架构审查（`/tmp/architecture-review-*.html`）识别 5 个深化候选。候选 1（tts playback session）、3（llm_sse 拆 module）、4（conn 文档化）已实施并提交。候选 2（agent_runtime turn_ctx_t）与 5（VA-TTS speak facade）为核心重构。

## 决策

暂缓候选 2、5，待项目引入单元测试后重新评估。

## 理由

### 候选 2 agent_runtime turn_ctx_t

硬约束：`agent_text_t` 无 free 函数，`s_reply` 是文件 static（data 分配后复用，不泄漏）。`finish_reply` 设 `*out_reply = agent_text_c_str(&s_reply)`，`run_chat`（voice_assistant.c）跨函数使用该指针。

- turn_ctx_t 栈上 → run_turn 返回后 out_reply 悬空。
- turn_ctx_t 堆分配 → 引入跨模块 free 责任（run_chat 不知 turn_ctx）。
- `s_reply` static 是 out_reply 生命周期的必要支撑，不可简单去除。

无单测：测试性收益（interface 是 test surface）无法兑现，只剩 locality 边际收益，核心重构风险 > 收益。

### 候选 5 VA-TTS speak facade

依赖候选 2 的 turn_ctx 传递，2 暂缓则 5 受阻。

## 后续

引入单元测试后，候选 2/5 的测试性收益可兑现。候选 2 需先解决 out_reply 生命周期（如新增 `agent_text_free` + 堆 turn_ctx 由 run_chat 释放，或 run_chat 复制 reply）。
