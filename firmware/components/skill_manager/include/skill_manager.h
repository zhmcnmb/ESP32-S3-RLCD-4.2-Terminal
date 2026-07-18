#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SD 声明式 Skill 索引：只接受 agent/skills/<name>/SKILL.md 的受限 front matter。
 * 启动读取元数据；正文在 load 时以标准 CRC-32/ISO-HDLC 表示的 content_crc32 复核后
 * 才返回。Skill 是不可信指导文本，永远不能注册工具；tools 字段必须是调用方传入的
 * 静态白名单子集。
 */
#define SKILL_MANAGER_CONTENT_MAX 2048
typedef struct {
    const char *const *allowed_tools;
    size_t allowed_tool_count;
} skill_manager_config_t;

/* SD 未挂载时也返回 ESP_OK，但 catalog 为空且 is_available 为 false。 */
esp_err_t skill_manager_init(const skill_manager_config_t *config);
bool skill_manager_is_available(void);

/* 返回模块持有的只读目录文本；为空表示没有通过校验的 Skill。 */
const char *skill_manager_catalog(void);

/* name 只接受索引中已验证的名称；out 由调用方持有。 */
esp_err_t skill_manager_load(const char *name, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
