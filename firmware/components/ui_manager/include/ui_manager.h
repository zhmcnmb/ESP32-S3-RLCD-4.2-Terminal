#pragma once

#include "esp_err.h"
#include "ui_page.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 页面导航与渲染调度。
 *
 * 交互约定:
 *   KEY 短按       -> 切换到下一页
 *   BOOT 短按      -> UI_KEY_ACTION 下发给当前页
 *   BOOT 长按(1s)  -> UI_KEY_BACK   下发给当前页
 *
 * 任务: ui_task, 栈4096, 优先级5 (登记于 docs/coding-standards.md 第6节)
 */

#define UI_MAX_PAGES 8

/* 依赖 board_init() 与 st7305_init() 已完成 */
esp_err_t ui_manager_init(void);

/* 注册页面。page 必须是静态生命周期。按注册顺序循环切换 */
esp_err_t ui_manager_register_page(const ui_page_t *page);

/* 启动UI任务，进入首个已注册页面 */
esp_err_t ui_manager_start(void);

/* 主动跳页 */
void ui_manager_goto(int index);

#ifdef __cplusplus
}
#endif
