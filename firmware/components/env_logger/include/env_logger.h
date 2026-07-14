#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 温湿度历史记录服务: 后台任务定期采样SHTC3，写入内存环形缓冲(供趋势页画图)，
 * SD卡已挂载时同步追加CSV(经 storage_sd，未挂载时只保留内存缓冲，不阻断运行)。
 * 依赖 shtc3_init() 已完成；SD卡是可选依赖，未挂载时降级为纯内存模式。
 *
 * 任务: env_logger_task, 栈4096, 优先级3 (登记于 docs/coding-standards.md 第6节)
 */

#define ENV_LOGGER_HISTORY_LEN 60 /* 环形缓冲容量: 60点 × 60s采样 = 1小时趋势窗口 */

typedef struct {
    time_t timestamp;
    float temperature;
    float humidity;
} env_logger_sample_t;

/* 启动后台采样任务。CSV文件: /sdcard/env.csv，未挂载SD卡时跳过落盘 */
esp_err_t env_logger_init(void);

/* 拷贝最近 count 条历史(按时间升序，最旧的在前)到 out。
 * 返回实际拷贝条数(<=count，环形缓冲未填满时可能小于count)。 */
int env_logger_get_history(env_logger_sample_t *out, int count);

/* 已采集样本总数(含被环形缓冲淘汰的)，用于趋势页显示"共记录N条" */
uint32_t env_logger_sample_count(void);

#ifdef __cplusplus
}
#endif
