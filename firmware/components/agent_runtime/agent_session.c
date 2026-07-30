/*
 * agent_session.c - 当前会话 JSONL 持久化与受限上下文恢复。
 * SD 仅保存消息记录；送入模型的内存上下文始终限制在最近十条。
 */
#include "agent_runtime_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "storage_sd.h"

#define SESSION_PATH "agent/sessions/current.jsonl"
#define SESSION_READ_CHUNK 1024
#define SESSION_RECOVERY_BYTES (32 * 1024)
#define SESSION_LINE_INITIAL_CAP 512

static const char *TAG = "agent_session";

typedef struct {
    agent_role_t role;
    char content[AGENT_CONTEXT_TEXT_MAX];
} session_context_t;

typedef struct {
    session_context_t entries[AGENT_CONTEXT_HISTORY_MAX];
    int head;
    int count;
    bool initialized;
} session_state_t;

static session_state_t s_session;

static const char *role_name(agent_role_t role)
{
    switch (role) {
    case AGENT_ROLE_SYSTEM: return "system";
    case AGENT_ROLE_USER: return "user";
    case AGENT_ROLE_ASSISTANT: return "assistant";
    case AGENT_ROLE_TOOL: return "tool";
    default: return NULL;
    }
}

static bool parse_role(const char *name, agent_role_t *out)
{
    if (!name || !out) {
        return false;
    }
    if (strcmp(name, "user") == 0) {
        *out = AGENT_ROLE_USER;
    } else if (strcmp(name, "assistant") == 0) {
        *out = AGENT_ROLE_ASSISTANT;
    } else {
        return false;
    }
    return true;
}

/* 截短上下文时回退到完整 UTF-8 字符边界，不污染下一次模型请求。 */
static size_t utf8_prefix_len(const char *text, size_t limit)
{
    size_t len = strlen(text);
    if (len <= limit) {
        return len;
    }
    size_t end = limit;
    while (end > 0 && (((unsigned char)text[end] & 0xc0) == 0x80)) {
        end--;
    }
    return end;
}

static void cache_context(agent_role_t role, const char *content)
{
    if (!content || (role != AGENT_ROLE_USER && role != AGENT_ROLE_ASSISTANT)) {
        return;
    }
    int index = (s_session.head + s_session.count) % AGENT_CONTEXT_HISTORY_MAX;
    if (s_session.count == AGENT_CONTEXT_HISTORY_MAX) {
        index = s_session.head;
        s_session.head = (s_session.head + 1) % AGENT_CONTEXT_HISTORY_MAX;
    } else {
        s_session.count++;
    }
    size_t len = utf8_prefix_len(content, sizeof(s_session.entries[index].content) - 1);
    s_session.entries[index].role = role;
    memcpy(s_session.entries[index].content, content, len);
    s_session.entries[index].content[len] = '\0';
}

static uint32_t calculate_crc(const char *text)
{
    return esp_crc32_le(0, (const uint8_t *)text, (uint32_t)strlen(text));
}

static void persist_record(cJSON *record)
{
    if (!record) {
        return;
    }
    if (!storage_sd_is_mounted()) {
        cJSON_Delete(record);
        return;
    }
    char *canonical = cJSON_PrintUnformatted(record);
    if (!canonical) {
        cJSON_Delete(record);
        return;
    }
    cJSON_AddNumberToObject(record, "crc32", calculate_crc(canonical));
    free(canonical);
    char *line = cJSON_PrintUnformatted(record);
    cJSON_Delete(record);
    if (!line) {
        return;
    }
    esp_err_t err = storage_sd_append_line(SESSION_PATH, line);
    free(line);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "会话写入失败: %s，继续使用 RAM 短会话", esp_err_to_name(err));
    }
}

static cJSON *new_record(agent_role_t role, const char *content, const char *tool_call_id)
{
    const char *name = role_name(role);
    if (!name) {
        return NULL;
    }
    cJSON *record = cJSON_CreateObject();
    if (!record) {
        return NULL;
    }
    if (!cJSON_AddNumberToObject(record, "v", 1)
        || !cJSON_AddStringToObject(record, "role", name)) {
        cJSON_Delete(record);
        return NULL;
    }
    if (content) {
        if (!cJSON_AddStringToObject(record, "content", content)) {
            cJSON_Delete(record);
            return NULL;
        }
    } else {
        if (!cJSON_AddNullToObject(record, "content")) {
            cJSON_Delete(record);
            return NULL;
        }
    }
    if (tool_call_id) {
        if (!cJSON_AddStringToObject(record, "tool_call_id", tool_call_id)) {
            cJSON_Delete(record);
            return NULL;
        }
    }
    if (!cJSON_AddNumberToObject(record, "timestamp", (double)time(NULL))) {
        cJSON_Delete(record);
        return NULL;
    }
    return record;
}

void agent_session_record_message(agent_role_t role, const char *content,
                                  const char *tool_call_id, bool include_context)
{
    if (include_context) {
        cache_context(role, content);
    }
    persist_record(new_record(role, content, tool_call_id));
}

void agent_session_record_tool_calls(const llm_tool_call_t *calls, int count)
{
    if (!calls || count <= 0 || count > LLM_MAX_TOOL_CALLS) {
        return;
    }
    cJSON *record = new_record(AGENT_ROLE_ASSISTANT, NULL, NULL);
    cJSON *array = record ? cJSON_AddArrayToObject(record, "tool_calls") : NULL;
    if (!array) {
        cJSON_Delete(record);
        return;
    }
    for (int i = 0; i < count; i++) {
        cJSON *call = cJSON_CreateObject();
        cJSON *function = call ? cJSON_AddObjectToObject(call, "function") : NULL;
        if (!function) {
            cJSON_Delete(call);
            cJSON_Delete(record);
            return;
        }
        cJSON_AddStringToObject(call, "id", calls[i].id);
        cJSON_AddStringToObject(call, "type", "function");
        cJSON_AddStringToObject(function, "name", calls[i].name);
        cJSON_AddStringToObject(function, "arguments", calls[i].arguments);
        if (!cJSON_AddItemToArray(array, call)) {
            cJSON_Delete(call);
            cJSON_Delete(record);
            return;
        }
    }
    persist_record(record);
}

static bool record_crc_valid(cJSON *record)
{
    cJSON *stored_crc = cJSON_DetachItemFromObject(record, "crc32");
    if (!cJSON_IsNumber(stored_crc)) {
        cJSON_Delete(stored_crc);
        return false;
    }
    char *canonical = cJSON_PrintUnformatted(record);
    bool valid = canonical && (uint32_t)stored_crc->valuedouble == calculate_crc(canonical);
    free(canonical);
    cJSON_Delete(stored_crc);
    return valid;
}

static void load_line(char *line)
{
    cJSON *record = cJSON_Parse(line);
    if (!record || !record_crc_valid(record)) {
        cJSON_Delete(record);
        return;
    }
    cJSON *role = cJSON_GetObjectItem(record, "role");
    cJSON *content = cJSON_GetObjectItem(record, "content");
    cJSON *calls = cJSON_GetObjectItem(record, "tool_calls");
    agent_role_t parsed_role;
    if (cJSON_IsString(role) && cJSON_IsString(content) && !cJSON_IsArray(calls)
        && parse_role(role->valuestring, &parsed_role)) {
        cache_context(parsed_role, content->valuestring);
    }
    cJSON_Delete(record);
}

static esp_err_t grow_line(char **line, size_t *capacity, size_t need)
{
    if (need == SIZE_MAX) {
        return ESP_ERR_NO_MEM;
    }
    size_t required = need + 1;
    size_t cap = *capacity ? *capacity : SESSION_LINE_INITIAL_CAP;
    while (cap < required) {
        if (cap > SIZE_MAX / 2) {
            cap = required;
            break;
        }
        cap *= 2;
    }
    char *grown = heap_caps_realloc(*line, cap, MALLOC_CAP_SPIRAM);
    if (!grown) {
        return ESP_ERR_NO_MEM;
    }
    *line = grown;
    *capacity = cap;
    return ESP_OK;
}

/* 当前会话可能持续追加。启动只从尾部恢复，避免用 256B 小读把多 MB 历史
 * 变成数千次 FATFS open/fseek；跳过的前缀不会影响工作记忆的最近 10 条。 */
static void load_from_storage(void)
{
    uint32_t file_size = 0;
    if (storage_sd_get_file_size(SESSION_PATH, &file_size) != ESP_OK) {
        return;
    }
    uint32_t offset = file_size > SESSION_RECOVERY_BYTES
                      ? file_size - SESSION_RECOVERY_BYTES : 0;
    bool discard_partial_line = offset != 0;
    if (discard_partial_line) {
        ESP_LOGI(TAG, "会话文件%uB，仅恢复末尾%uB", (unsigned)file_size,
                 (unsigned)SESSION_RECOVERY_BYTES);
    }

    char *line = NULL;
    size_t line_len = 0;
    size_t line_cap = 0;
    uint8_t chunk[SESSION_READ_CHUNK];
    for (;;) {
        int read_len = storage_sd_read_chunk(SESSION_PATH, offset, chunk, sizeof(chunk));
        if (read_len <= 0) {
            break;
        }
        offset += (uint32_t)read_len;
        for (int i = 0; i < read_len; i++) {
            if (discard_partial_line) {
                discard_partial_line = chunk[i] != '\n';
            } else if (chunk[i] == '\n') {
                if (line) {
                    line[line_len] = '\0';
                    load_line(line);
                    line_len = 0;
                }
            } else if (grow_line(&line, &line_cap, line_len + 1) == ESP_OK) {
                line[line_len++] = (char)chunk[i];
            } else {
                ESP_LOGW(TAG, "会话恢复内存不足，停止读取旧记录");
                free(line);
                return;
            }
        }
    }
    free(line); /* 无换行的末行被视为掉电撕裂记录，故意忽略。 */
}

void agent_session_init(void)
{
    if (s_session.initialized) {
        return;
    }
    memset(&s_session, 0, sizeof(s_session));
    s_session.initialized = true;
    if (!storage_sd_is_mounted()) {
        ESP_LOGW(TAG, "SD 不可用，使用 RAM 短会话");
        return;
    }
    load_from_storage();
    ESP_LOGI(TAG, "已恢复 %d 条上下文消息", s_session.count);
}

int agent_session_copy_context(llm_message_t *out, int capacity)
{
    if (!out || capacity <= 0) {
        return 0;
    }
    int count = s_session.count < capacity ? s_session.count : capacity;
    int start = (s_session.head + s_session.count - count) % AGENT_CONTEXT_HISTORY_MAX;
    for (int i = 0; i < count; i++) {
        session_context_t *entry = &s_session.entries[(start + i) % AGENT_CONTEXT_HISTORY_MAX];
        out[i] = (llm_message_t){
            .role = entry->role,
            .content = entry->content,
            .tool_call_id = NULL,
            .tool_calls = NULL,
            .tool_call_count = 0,
        };
    }
    return count;
}
