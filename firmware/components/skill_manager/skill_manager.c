/* skill_manager.c - 受限 front matter 的 SD Skill 索引与按需复核。 */
#include "skill_manager.h"
#include "skill_manager_internal.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "storage_sd.h"

static const char *TAG = "skill_manager";

#define SKILL_ROOT "agent/skills"
#define SKILL_MAX_COUNT 8
#define SKILL_NAME_MAX 33
#define SKILL_DESCRIPTION_MAX 161
#define SKILL_VERSION_MAX 17
#define SKILL_TOOLS_MAX 129
#define SKILL_HEADER_MAX 768
#define SKILL_FILE_MAX 4096
#define SKILL_CONTENT_MAX 2048
#define SKILL_CATALOG_MAX 2048
#define SKILL_ALLOWED_MAX 8

typedef struct {
    char directory[SKILL_NAME_MAX];
    char name[SKILL_NAME_MAX];
    char description[SKILL_DESCRIPTION_MAX];
    char version[SKILL_VERSION_MAX];
    char tools[SKILL_TOOLS_MAX];
    uint32_t content_crc32;
} skill_entry_t;

typedef struct {
    skill_entry_t entries[SKILL_MAX_COUNT];
    int count;
    const char *allowed_tools[SKILL_ALLOWED_MAX];
    size_t allowed_count;
    char catalog[SKILL_CATALOG_MAX];
    bool initialized;
    bool available;
} skill_state_t;

static skill_state_t s_state;

static void copy_text(char *out, size_t out_size, const char *text)
{
    size_t len = strlen(text);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, text, len);
    out[len] = '\0';
}

static char *trim(char *text)
{
    while (*text && isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}

static char *next_line(char **cursor)
{
    if (!cursor || !*cursor || !**cursor) return NULL;
    char *line = *cursor;
    char *end = strchr(line, '\n');
    if (end) {
        *end = '\0';
        *cursor = end + 1;
    } else {
        *cursor = line + strlen(line);
    }
    if (*line && line[strlen(line) - 1] == '\r') line[strlen(line) - 1] = '\0';
    return line;
}

static bool valid_name(const char *name)
{
    size_t len = name ? strlen(name) : 0;
    if (len == 0 || len >= SKILL_NAME_MAX || name[0] == '-' || name[len - 1] == '-') return false;
    for (size_t i = 0; i < len; i++) {
        if (!(islower((unsigned char)name[i]) || isdigit((unsigned char)name[i]) || name[i] == '-')) return false;
    }
    return true;
}

static bool tool_is_allowed(const char *name)
{
    for (size_t i = 0; i < s_state.allowed_count; i++) {
        if (strcmp(name, s_state.allowed_tools[i]) == 0) return true;
    }
    return false;
}

static bool tools_are_allowed(const char *tools)
{
    char names[SKILL_TOOLS_MAX];
    copy_text(names, sizeof(names), tools);
    char *save = NULL;
    char *name = strtok_r(names, ",", &save);
    if (!name) return false;
    while (name) {
        if (!tool_is_allowed(trim(name))) return false;
        name = strtok_r(NULL, ",", &save);
    }
    return true;
}

static bool parse_crc32(const char *text, uint32_t *out)
{
    if (!text || strlen(text) != 8) return false;
    for (int i = 0; i < 8; i++) if (!isxdigit((unsigned char)text[i])) return false;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 16);
    if (!end || *end || value > UINT32_MAX) return false;
    *out = (uint32_t)value;
    return true;
}

/* Skill 文件由常见工具生成，沿用 CRC-32/ISO-HDLC 的公开表示；ROM API 返回内部状态。 */
static uint32_t skill_content_crc32(const char *content, size_t content_len)
{
    return ~esp_crc32_le(UINT32_MAX, (const uint8_t *)content, content_len);
}

static bool parse_header(char *text, skill_entry_t *entry, char **out_content)
{
    bool has_name = false, has_description = false, has_version = false, has_tools = false, has_crc = false;
    char *cursor = text;
    char *line = next_line(&cursor);
    if (!line || strcmp(line, "---") != 0) return false;
    while ((line = next_line(&cursor)) != NULL) {
        if (strcmp(line, "---") == 0) {
            if (out_content) *out_content = cursor;
            return has_name && has_description && has_version && has_tools && has_crc
                   && valid_name(entry->name) && tools_are_allowed(entry->tools);
        }
        char *separator = strchr(line, ':');
        if (!separator) return false;
        *separator = '\0';
        char *key = trim(line);
        char *value = trim(separator + 1);
        if (strcmp(key, "name") == 0 && !has_name && *value) {
            copy_text(entry->name, sizeof(entry->name), value);
            has_name = true;
        } else if (strcmp(key, "description") == 0 && !has_description && *value) {
            copy_text(entry->description, sizeof(entry->description), value);
            has_description = true;
        } else if (strcmp(key, "version") == 0 && !has_version && *value) {
            copy_text(entry->version, sizeof(entry->version), value);
            has_version = true;
        } else if (strcmp(key, "tools") == 0 && !has_tools && *value) {
            copy_text(entry->tools, sizeof(entry->tools), value);
            has_tools = true;
        } else if (strcmp(key, "content_crc32") == 0 && !has_crc && parse_crc32(value, &entry->content_crc32)) {
            has_crc = true;
        } else {
            return false;
        }
    }
    return false;
}

static esp_err_t collect_directory(const char *name, void *ctx)
{
    char (*directories)[SKILL_NAME_MAX] = ctx;
    if (!valid_name(name)) return ESP_OK;
    for (int i = 0; i < SKILL_MAX_COUNT; i++) {
        if (directories[i][0] == '\0') {
            copy_text(directories[i], sizeof(directories[i]), name);
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_SIZE;
}

static esp_err_t read_header(const char *directory, skill_entry_t *entry)
{
    char path[96];
    int path_len = snprintf(path, sizeof(path), "%s/%s/SKILL.md", SKILL_ROOT, directory);
    if (path_len < 0 || (size_t)path_len >= sizeof(path)) return ESP_ERR_INVALID_SIZE;
    uint32_t size = 0;
    if (storage_sd_get_file_size(path, &size) != ESP_OK || size > SKILL_FILE_MAX) return ESP_ERR_NOT_FOUND;
    char header[SKILL_HEADER_MAX + 1];
    int read_len = storage_sd_read_chunk(path, 0, header, SKILL_HEADER_MAX);
    if (read_len <= 0) return ESP_FAIL;
    header[read_len] = '\0';
    entry->directory[0] = '\0';
    if (!parse_header(header, entry, NULL) || strcmp(directory, entry->name) != 0) return ESP_ERR_INVALID_RESPONSE;
    copy_text(entry->directory, sizeof(entry->directory), directory);
    return ESP_OK;
}

static void rebuild_catalog(void)
{
    s_state.catalog[0] = '\0';
    for (int i = 0; i < s_state.count; i++) {
        skill_entry_t *entry = &s_state.entries[i];
        size_t used = strlen(s_state.catalog);
        int written = snprintf(s_state.catalog + used, sizeof(s_state.catalog) - used,
                               "- %s (v%s): %s [tools: %s]\n", entry->name, entry->version,
                               entry->description, entry->tools);
        if (written < 0 || (size_t)written >= sizeof(s_state.catalog) - used) {
            s_state.catalog[used] = '\0';
            break;
        }
    }
}

static bool same_metadata(const skill_entry_t *a, const skill_entry_t *b)
{
    return strcmp(a->directory, b->directory) == 0 && strcmp(a->name, b->name) == 0
           && strcmp(a->version, b->version) == 0 && strcmp(a->tools, b->tools) == 0
           && a->content_crc32 == b->content_crc32;
}

esp_err_t skill_manager_init(const skill_manager_config_t *config)
{
    if (!config || !config->allowed_tools || config->allowed_tool_count == 0
        || config->allowed_tool_count > SKILL_ALLOWED_MAX) return ESP_ERR_INVALID_ARG;
    memset(&s_state, 0, sizeof(s_state));
    memcpy(s_state.allowed_tools, config->allowed_tools,
           config->allowed_tool_count * sizeof(s_state.allowed_tools[0]));
    s_state.allowed_count = config->allowed_tool_count;
    s_state.initialized = true;
    if (!storage_sd_is_mounted()) return ESP_OK;
    esp_err_t seed_err = skill_defaults_seed();
    if (seed_err != ESP_OK) return seed_err;

    char directories[SKILL_MAX_COUNT][SKILL_NAME_MAX] = {0};
    esp_err_t list_err = storage_sd_list_dir(SKILL_ROOT, collect_directory, directories);
    if (list_err != ESP_OK) return list_err;
    for (int i = 0; i < SKILL_MAX_COUNT; i++) {
        if (!directories[i][0]) continue;
        skill_entry_t entry = {0};
        esp_err_t err = read_header(directories[i], &entry);
        if (err != ESP_OK) continue;
        bool duplicate = false;
        for (int j = 0; j < s_state.count; j++) duplicate |= strcmp(s_state.entries[j].name, entry.name) == 0;
        if (!duplicate) s_state.entries[s_state.count++] = entry;
    }
    s_state.available = s_state.count > 0;
    rebuild_catalog();
    ESP_LOGI(TAG, "已索引 %d 个Skill", s_state.count);
    return ESP_OK;
}

bool skill_manager_is_available(void)
{
    return s_state.initialized && s_state.available && storage_sd_is_mounted();
}

const char *skill_manager_catalog(void)
{
    return s_state.catalog;
}

esp_err_t skill_manager_load(const char *name, char *out, size_t out_size)
{
    if (!skill_manager_is_available() || !valid_name(name) || !out || out_size == 0) return ESP_ERR_INVALID_ARG;
    const skill_entry_t *indexed = NULL;
    for (int i = 0; i < s_state.count; i++) if (strcmp(name, s_state.entries[i].name) == 0) indexed = &s_state.entries[i];
    if (!indexed) return ESP_ERR_NOT_FOUND;

    char path[96];
    snprintf(path, sizeof(path), "%s/%s/SKILL.md", SKILL_ROOT, indexed->directory);
    uint32_t file_size = 0;
    if (storage_sd_get_file_size(path, &file_size) != ESP_OK || file_size > SKILL_FILE_MAX) return ESP_ERR_INVALID_SIZE;
    char *file = heap_caps_calloc(1, (size_t)file_size + 1, MALLOC_CAP_SPIRAM);
    if (!file) return ESP_ERR_NO_MEM;
    int read_len = storage_sd_read_chunk(path, 0, file, file_size);
    if (read_len != (int)file_size) {
        free(file);
        return ESP_FAIL;
    }
    skill_entry_t current = {0};
    char *content = NULL;
    copy_text(current.directory, sizeof(current.directory), indexed->directory);
    bool valid = parse_header(file, &current, &content) && same_metadata(indexed, &current);
    size_t content_len = valid && content ? strlen(content) : 0;
    valid = valid && content_len > 0 && content_len <= SKILL_CONTENT_MAX
            && skill_content_crc32(content, content_len) == indexed->content_crc32;
    if (!valid || content_len >= out_size) {
        free(file);
        return valid ? ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_CRC;
    }
    memcpy(out, content, content_len + 1);
    free(file);
    return ESP_OK;
}
