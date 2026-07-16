#pragma once

#include "sys_state.h"

/*
 * UI V2 共享视觉原语：页头/页脚/分隔线/空状态。只在 ui_pages 内部使用，
 * 不对外暴露。该视觉方案将在 P5.2 按 docs/voice-agent-design.md 第9节收敛。
 * 四个已迁移页面(home/env_monitor/quota/weather)统一调用这几个函数，
 * 只有主体内容(y=44..247)各自实现，保证页头/页脚/状态图标位置一致。
 */

/* 页头: 左侧标题(中文scale2)，右侧SD/WiFi/电池图标(固定顺序、固定占位)，
 * 底部1px分隔线。数据不可用时图标改变内部形态(叉/空电量)，不改变占位宽度 */
void ui_v2_draw_header(const char *title, const sys_state_snapshot_t *snap);

/* 页脚: 分隔线 + "KEY 翻页  长按说话  BOOT <action_label>"。
 * action_label是BOOT短按在当前页的具体动作文案，如"刷新" */
void ui_v2_draw_footer(const char *action_label);

/* 通用分隔线，1px，与页头/页脚同宽(x=20..380) */
void ui_v2_hline(int y);

/* 统一空状态：居中主文案(scale2) + 可选居中副文案(scale1，y+40处)。
 * 用于"正在获取"/"网络不可用"/"数据暂不可用"等场景，secondary可传NULL */
void ui_v2_draw_empty_state(int y, const char *primary, const char *secondary);
