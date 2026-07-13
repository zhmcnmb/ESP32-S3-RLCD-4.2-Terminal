#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Micro SD卡 (SDMMC外设 1-bit模式，非SPI，与显示屏SPI无冲突)。
 * 挂载点 /sdcard，FAT文件系统。
 *
 * 卡未插入时 storage_sd_init() 返回错误但不影响系统运行——
 * SD卡是可选外设，调用方应降级处理而非中止启动。
 */

#define STORAGE_SD_MOUNT_POINT "/sdcard"

typedef struct {
    uint64_t total_bytes;
    uint64_t used_bytes;
    char name[16];   /* 卡名 */
} storage_sd_info_t;

/* 挂载SD卡。未插卡/挂载失败返回错误 */
esp_err_t storage_sd_init(void);

bool storage_sd_is_mounted(void);

/* 容量信息，未挂载时返回错误 */
esp_err_t storage_sd_get_info(storage_sd_info_t *out);

/* 追加一行文本到文件(自动加换行)。路径相对挂载点，如 "env.csv" */
esp_err_t storage_sd_append_line(const char *rel_path, const char *line);

#ifdef __cplusplus
}
#endif
