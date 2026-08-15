#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 主动提醒调度 Module：把一次/周期提醒、掉电恢复、可信时间门槛和单槽投递收进内部。
 * 调用方只添加、取消或列出任务；到期动作经 sys_state 邮箱交给 voice_assistant，
 * scheduler 不依赖语音实现，因此两边可独立演进。
 */

#define SCHEDULER_TEXT_MAX 96
#define SCHEDULER_TASK_MAX 16

typedef enum {
    SCHED_ONCE = 0,
    SCHED_INTERVAL,
} sched_kind_t;

typedef struct {
    uint32_t id;
    sched_kind_t kind;
    time_t fire_at;        /* 下次触发UTC epoch */
    uint32_t interval_s;   /* SCHED_ONCE 时为0 */
    char text[SCHEDULER_TEXT_MAX];
} scheduler_task_info_t;

/* 初始化任务表与每秒检查任务。SD不可用时仍成功，以RAM任务降级(重启即失)。 */
esp_err_t scheduler_init(void);
bool scheduler_is_available(void);

/* 添加一次/周期提醒。可信时间不可用时返回ESP_ERR_INVALID_STATE；delay_s必须>0，
 * 周期提醒的interval_s必须>0；满16条返回ESP_ERR_NO_MEM。 */
esp_err_t scheduler_add(sched_kind_t kind, uint32_t delay_s, uint32_t interval_s,
                        const char *text, uint32_t *out_id);

/* 取消任务；不存在返回ESP_ERR_NOT_FOUND。 */
esp_err_t scheduler_cancel(uint32_t id);

/* 列出当前任务；cap不足时尽量复制并返回ESP_ERR_INVALID_SIZE，out_count为实际复制数。 */
esp_err_t scheduler_list(scheduler_task_info_t *out, size_t cap, size_t *out_count);

#ifdef __cplusplus
}
#endif
