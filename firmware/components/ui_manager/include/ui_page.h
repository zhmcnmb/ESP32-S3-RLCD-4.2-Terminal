#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 页面接口契约。所有功能页面(时钟天气/环境监测/语音等)都实现这个结构体，
 * 通过 ui_manager_register_page() 注册。
 *
 * 约定:
 *  - 页面只负责渲染和交互，只读 sys_state 快照、提交命令，不准自己发HTTP/读传感器
 *  - 页面之间不准互相include，共享状态走 sys_state
 *  - 新增页面不需要修改 ui_manager 的任何代码
 */

typedef enum {
    UI_KEY_NEXT,   /* KEY短按: 切换到下一页 (由ui_manager处理，不下发给页面) */
    UI_KEY_PREV,   /* KEY长按: 上一页(ui_manager处理) */
    UI_KEY_ACTION, /* BOOT短按: 空闲时手动触发语音, 否则页面内动作 */
    UI_KEY_BACK,   /* BOOT长按: 返回/取消 */
} ui_key_event_t;

typedef struct {
    const char *name;

    /* 进入页面: 申请资源、订阅事件。可为NULL */
    void (*on_enter)(void);

    /* 离开页面: 释放资源、退订事件。可为NULL */
    void (*on_exit)(void);

    /* 周期回调: 拉取最新数据并重绘。可为NULL。
     * 调用间隔由 ui_manager 统一控制，页面不要自己起定时器 */
    void (*on_tick)(void);

    /* 按键事件: 返回true表示已消费，false则交回ui_manager处理。可为NULL */
    bool (*on_key)(ui_key_event_t evt);
} ui_page_t;

#ifdef __cplusplus
}
#endif
