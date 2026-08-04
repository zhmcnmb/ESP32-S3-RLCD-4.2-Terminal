#pragma once

#include "esp_err.h"
#include "ui_page.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 页面导航与渲染调度。GPIO消抖/长短按判定归 input_manager 所有，本模块只消费
 * 其发布的语义事件，不读取GPIO。
 *
 * 交互约定:
 *   NEXT (KEY短按)    -> 切换到下一页
 *   PREV (KEY长按)    -> 切换到上一页
 *   ACTION (BOOT短按) -> 语音空闲时 push-to-talk；否则 UI_KEY_ACTION 下发给当前页
 *   BACK (BOOT长按)   -> 语音活动时全局取消，否则 UI_KEY_BACK 下发给当前页
 *
 * 任务: ui_task, 栈4096, 优先级5 (登记于 docs/coding-standards.md 第7节)
 */

#define UI_MAX_PAGES 8

/* 依赖 board_init()、st7305_init() 已完成；内部会向 input_manager 注册事件回调，
 * 调用顺序与 input_manager_init() 无先后要求(回调注册前触发的事件会被忽略) */
esp_err_t ui_manager_init(void);

/* 注册页面。page 必须是静态生命周期。按注册顺序循环切换 */
esp_err_t ui_manager_register_page(const ui_page_t *page);

/* 启动UI任务，进入首个已注册页面 */
esp_err_t ui_manager_start(void);

#ifdef __cplusplus
}
#endif
