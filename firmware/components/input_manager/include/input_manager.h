#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * KEY/BOOT物理按键的唯一所有者：消抖、长短按判定都在这里，对外只发布语义事件。
 * 依赖 board_init() 已完成(拿 board_btn_key_pressed()/board_btn_boot_pressed())。
 *
 * 语义:
 *   KEY 短按 -> NEXT；KEY 长按 -> PREV
 *   BOOT 短按 -> ACTION；BOOT 长按 -> BACK（上层按上下文解读为取消）
 * 手势只在按键释放(短按)或达到长按阈值(长按)时发布一次；长按成立后释放不再补发短按。
 *
 * 任务: input_task, 栈2048, 优先级4
 */

typedef enum {
    INPUT_EVENT_NEXT,
    INPUT_EVENT_PREV,
    INPUT_EVENT_ACTION,
    INPUT_EVENT_BACK,
} input_event_t;

typedef void (*input_event_cb_t)(input_event_t evt);

/* 启动GPIO轮询任务。回调在input_task上下文同步执行，必须简短、非阻塞 */
esp_err_t input_manager_init(void);

/* 注册语义事件回调，同一时刻只支持一个订阅者(当前唯一调用方是ui_manager) */
esp_err_t input_manager_set_handler(input_event_cb_t cb);

#ifdef __cplusplus
}
#endif
