/* agent_memory_tools.c - 长期记忆的受限工具 Adapter。 */
#include "agent_runtime_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "memory_engine.h"

static bool s_allow_remember;
static bool s_allow_forget;

static const llm_tool_def_t s_definitions[] = {
    {.name = "memory_search", .description = "检索与问题相关的已保存长期记忆。",
     .parameters_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"maxLength\":192},\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8}},\"required\":[\"query\",\"limit\"],\"additionalProperties\":false}"},
    {.name = "list_recent_memories", .description = "列出最近保存的长期记忆及其编号。",
     .parameters_json = "{\"type\":\"object\",\"properties\":{\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8}},\"required\":[\"limit\"],\"additionalProperties\":false}"},
    {.name = "remember", .description = "仅在用户明确要求记住时保存简短长期记忆。",
     .parameters_json = "{\"type\":\"object\",\"properties\":{\"content\":{\"type\":\"string\",\"maxLength\":240},\"type\":{\"type\":\"string\",\"enum\":[\"profile\",\"preference\",\"fact\",\"correction\",\"episode\"]},\"ttl_seconds\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":31536000}},\"required\":[\"content\",\"type\"],\"additionalProperties\":false}"},
    {.name = "forget_memory", .description = "仅在用户明确要求忘记时删除指定编号的长期记忆。",
     .parameters_json = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\",\"minimum\":1}},\"required\":[\"id\"],\"additionalProperties\":false}"},
};

static bool user_requested(const char *text, const char *first, const char *second)
{
    return text && (strstr(text, first) || strstr(text, second));
}

void agent_memory_tools_prepare(const char *user_text)
{
    s_allow_remember = user_requested(user_text, "记住", "记下");
    s_allow_forget = user_requested(user_text, "忘记", "删除记忆");
}

static const char *type_name(memory_type_t type)
{
    static const char *const names[] = {"profile", "preference", "fact", "correction", "episode"};
    return type >= MEMORY_TYPE_PROFILE && type <= MEMORY_TYPE_EPISODE ? names[type] : "unknown";
}

static bool parse_type(const char *name, memory_type_t *out)
{
    for (int type = MEMORY_TYPE_PROFILE; type <= MEMORY_TYPE_EPISODE; type++) {
        if (strcmp(name, type_name((memory_type_t)type)) == 0) {
            *out = (memory_type_t)type;
            return true;
        }
    }
    return false;
}

static bool has_fields(const cJSON *args, const char *first, const char *second, const char *optional)
{
    if (!cJSON_IsObject(args)) return false;
    bool got_first = false;
    bool got_second = strcmp(first, second) == 0;
    bool got_optional = false;
    for (const cJSON *item = args->child; item; item = item->next) {
        if (!item->string) return false;
        if (strcmp(item->string, first) == 0 && !got_first) got_first = true;
        else if (strcmp(item->string, second) == 0 && !got_second) got_second = true;
        else if (optional && strcmp(item->string, optional) == 0 && !got_optional) got_optional = true;
        else return false;
    }
    return got_first && got_second;
}

static bool parse_uint(const cJSON *args, const char *name, uint32_t min, uint32_t max, uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(args, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < min || item->valuedouble > max
        || item->valuedouble != (double)item->valueint) return false;
    *out = (uint32_t)item->valueint;
    return true;
}

static esp_err_t context_to_json(const memory_context_t *context, cJSON **out)
{
    cJSON *json = cJSON_CreateObject();
    cJSON *memories = json ? cJSON_AddArrayToObject(json, "memories") : NULL;
    if (!memories || !cJSON_AddNumberToObject(json, "count", context->count)) {
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    for (uint8_t i = 0; i < context->count; i++) {
        const memory_record_t *record = &context->records[i];
        cJSON *item = cJSON_CreateObject();
        if (!item || !cJSON_AddNumberToObject(item, "id", record->id)
            || !cJSON_AddStringToObject(item, "type", type_name(record->type))
            || !cJSON_AddStringToObject(item, "content", record->content)
            || !cJSON_AddNumberToObject(item, "expires_at", (double)record->expires_at)) {
            cJSON_Delete(item);
            cJSON_Delete(json);
            return ESP_ERR_NO_MEM;
        }
        if (!cJSON_AddItemToArray(memories, item)) {
            cJSON_Delete(item);
            cJSON_Delete(json);
            return ESP_ERR_NO_MEM;
        }
    }
    *out = json;
    return ESP_OK;
}

static esp_err_t recall_to_json(const char *text, uint32_t limit, cJSON **out)
{
    memory_context_t *context = heap_caps_calloc(1, sizeof(*context), MALLOC_CAP_SPIRAM);
    if (!context) return ESP_ERR_NO_MEM;
    memory_query_t query = {
        .text = text, .limit = (uint8_t)limit, .max_bytes = MEMORY_ENGINE_CONTEXT_MAX,
    };
    esp_err_t err = memory_recall(&query, context);
    if (err == ESP_OK) err = context_to_json(context, out);
    free(context);
    return err;
}

static esp_err_t tool_search(const cJSON *args, cJSON **out)
{
    const cJSON *query = cJSON_GetObjectItemCaseSensitive(args, "query");
    uint32_t limit = 0;
    if (!out || !has_fields(args, "query", "limit", NULL) || !cJSON_IsString(query)
        || !query->valuestring[0] || strlen(query->valuestring) > 192
        || !parse_uint(args, "limit", 1, MEMORY_ENGINE_MAX_MATCHES, &limit)) return ESP_ERR_INVALID_ARG;
    return recall_to_json(query->valuestring, limit, out);
}

static esp_err_t tool_list_recent(const cJSON *args, cJSON **out)
{
    uint32_t limit = 0;
    if (!out || !has_fields(args, "limit", "limit", NULL)
        || !parse_uint(args, "limit", 1, MEMORY_ENGINE_MAX_MATCHES, &limit)) return ESP_ERR_INVALID_ARG;
    return recall_to_json("", limit, out);
}

static esp_err_t tool_remember(const cJSON *args, cJSON **out)
{
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(args, "content");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(args, "type");
    memory_type_t parsed_type;
    uint32_t ttl = 0;
    const cJSON *ttl_item = cJSON_GetObjectItemCaseSensitive(args, "ttl_seconds");
    if (!s_allow_remember || !out || !has_fields(args, "content", "type", "ttl_seconds")
        || !cJSON_IsString(content) || !cJSON_IsString(type) || !parse_type(type->valuestring, &parsed_type)
        || strlen(content->valuestring) > MEMORY_ENGINE_CONTENT_MAX
        || (ttl_item && !parse_uint(args, "ttl_seconds", 1, MEMORY_ENGINE_TTL_MAX_SECONDS, &ttl))) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *result = cJSON_CreateObject();
    if (!result || !cJSON_AddBoolToObject(result, "stored", true)) {
        cJSON_Delete(result);
        return ESP_ERR_NO_MEM;
    }
    memory_event_t event = {.type = parsed_type, .content = content->valuestring, .ttl_seconds = ttl};
    esp_err_t err = memory_record(&event);
    if (err != ESP_OK) {
        cJSON_Delete(result);
        return err;
    }
    *out = result;
    return ESP_OK;
}

static esp_err_t tool_forget(const cJSON *args, cJSON **out)
{
    uint32_t id = 0;
    if (!s_allow_forget || !out || !has_fields(args, "id", "id", NULL)
        || !parse_uint(args, "id", 1, UINT32_MAX, &id)) return ESP_ERR_INVALID_ARG;
    cJSON *result = cJSON_CreateObject();
    if (!result || !cJSON_AddBoolToObject(result, "forgotten", true)) {
        cJSON_Delete(result);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = memory_forget((memory_id_t)id);
    if (err != ESP_OK) {
        cJSON_Delete(result);
        return err;
    }
    *out = result;
    return ESP_OK;
}

const llm_tool_def_t *agent_memory_tools_get_definitions(int *out_count)
{
    static llm_tool_def_t active[4];
    int count = 0;
    if (memory_engine_is_available()) {
        active[count++] = s_definitions[0];
        active[count++] = s_definitions[1];
        if (s_allow_remember) active[count++] = s_definitions[2];
        if (s_allow_forget) active[count++] = s_definitions[3];
    }
    if (out_count) *out_count = count;
    return active;
}

bool agent_memory_tools_handles(const char *name)
{
    if (!memory_engine_is_available() || !name) return false;
    if (strcmp(name, "memory_search") == 0 || strcmp(name, "list_recent_memories") == 0) return true;
    if (strcmp(name, "remember") == 0) return s_allow_remember;
    return strcmp(name, "forget_memory") == 0 && s_allow_forget;
}

esp_err_t agent_memory_tools_execute(const char *name, const cJSON *args, cJSON **out)
{
    if (!agent_memory_tools_handles(name)) return ESP_ERR_NOT_FOUND;
    if (strcmp(name, "memory_search") == 0) return tool_search(args, out);
    if (strcmp(name, "list_recent_memories") == 0) return tool_list_recent(args, out);
    if (strcmp(name, "remember") == 0) return tool_remember(args, out);
    return tool_forget(args, out);
}
