#pragma once

#include <stdbool.h>
#include <stddef.h>
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
 *
 * 持久化 Interface：append、stream read、size/list 元数据读取、atomic replace。
 * 内部持一把互斥锁串行化所有文件操作，调用方不需要（也不应该）自己给 FATFS 访问加锁；
 * rel_path一律校验拒绝".."和绝对路径，防止越界写到挂载点之外。后续录音/会话/Skill/
 * 长期记忆(P6+)都通过这些操作访问SD，不再各自直接fopen。
 */

#define STORAGE_SD_MOUNT_POINT "/sdcard"

/* 挂载SD卡。未插卡/挂载失败返回错误 */
esp_err_t storage_sd_init(void);

bool storage_sd_is_mounted(void);

/* 追加一行文本到文件(自动加换行)。路径相对挂载点，如 "agent/sessions/current.jsonl"；
 * 缺失的父目录会在同一存储锁内自动创建。line必须指向RAM(栈/堆/.data)，不能是
 * flash直接映射地址——见下方atomic_replace的DMA说明，同一限制对append_line同样适用。 */
esp_err_t storage_sd_append_line(const char *rel_path, const char *line);

/* 从offset开始流式读取最多buf_len字节到buf。返回实际读取字节数，0表示
 * EOF/文件不存在/读取失败(用storage_sd_is_mounted()区分是否因为未挂载)。
 * 用于后续按块读取CSV/会话/记忆文件，不需要一次性把整个文件读进内存 */
int storage_sd_read_chunk(const char *rel_path, uint32_t offset, void *buf, size_t buf_len);

/* 读取相对文件的长度。不读取正文；文件不存在/未挂载时返回对应错误。 */
esp_err_t storage_sd_get_file_size(const char *rel_path, uint32_t *out_size);

/* 枚举相对目录下的直接子项，仅传回文件名而非完整路径；callback 在SD存储锁内执行，
 * 因而不得重入任何 storage_sd_* 函数，调用方应先收集名称再逐项读取。 */
typedef esp_err_t (*storage_sd_dir_entry_cb_t)(const char *name, void *ctx);
esp_err_t storage_sd_list_dir(const char *rel_dir, storage_sd_dir_entry_cb_t on_entry, void *ctx);

/* 原子替换文件全部内容：先写临时文件，成功后rename覆盖目标，避免写到一半
 * 掉电导致文件损坏或截断。用于配置/校验和/manifest等"整体替换"场景。
 *
 * data必须指向RAM(栈/堆/.data/.bss)，不能是flash直接映射地址(如
 * `EMBED_FILES`嵌入的只读资源、`static const`大数组)——SDMMC写入path经DMA，
 * DMA不能直接从flash cache映射地址取数：fwrite()会"成功"返回，但落盘内容
 * 全是0，不报错、不好排查(P5.0真机验证踩过)。调用方需要写flash常量时，
 * 先memcpy到堆/PSRAM缓冲区再传进来 */
esp_err_t storage_sd_atomic_replace(const char *rel_path, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
