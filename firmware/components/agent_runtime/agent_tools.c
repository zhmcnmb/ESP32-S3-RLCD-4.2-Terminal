/*
 * agent_tools.c - 静态白名单工具。
 * 设备快照、网页和 Skill 均经各自 Module Interface 获取，不让模型指定路径或网络目标。
 */
#include "agent_runtime_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "skill_manager.h"
#include "sys_state.h"
#include "web_client.h"


static const char *TAG = "agent_tools";
#define AGENT_ACTIVE_TOOL_MAX 13
typedef esp_err_t (*agent_tool_fn_t)(const cJSON *args, cJSON **result);

typedef struct {
    const char *name;
    agent_tool_fn_t execute;
} agent_tool_t;

static bool has_no_arguments(const cJSON *args)
{
    return cJSON_IsObject(args) && args->child == NULL;
}

static esp_err_t tool_device_status(const cJSON *args, cJSON **result)
{
    if (!has_no_arguments(args) || !result) {
        return ESP_ERR_INVALID_ARG;
    }
    sys_state_snapshot_t snapshot;
    sys_state_get_snapshot(&snapshot);
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    bool added = cJSON_AddBoolToObject(json, "time_valid", snapshot.time_valid)
                 && cJSON_AddBoolToObject(json, "wifi_connected", snapshot.wifi_connected)
                 && cJSON_AddBoolToObject(json, "sd_mounted", snapshot.sd_mounted)
                 && cJSON_AddBoolToObject(json, "battery_available", snapshot.battery_ok);
    if (snapshot.battery_ok) {
        added = added && cJSON_AddNumberToObject(json, "battery_percent", snapshot.battery_percent)
                && cJSON_AddBoolToObject(json, "battery_charging", snapshot.battery_charging);
    }
    if (!added) {
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    *result = json;
    return ESP_OK;
}

static esp_err_t tool_environment(const cJSON *args, cJSON **result)
{
    if (!has_no_arguments(args) || !result) {
        return ESP_ERR_INVALID_ARG;
    }
    sys_state_snapshot_t snapshot;
    sys_state_get_snapshot(&snapshot);
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    bool added = cJSON_AddBoolToObject(json, "available", snapshot.env_ok)
                 && cJSON_AddNumberToObject(json, "history_samples", snapshot.env_history_count);
    if (snapshot.env_ok) {
        added = added && cJSON_AddNumberToObject(json, "temperature_c", snapshot.temperature)
                && cJSON_AddNumberToObject(json, "humidity_percent", snapshot.humidity);
    }
    if (snapshot.env_history_count > 0) {
        float min_temp = snapshot.env_history[0].temperature;
        float max_temp = min_temp;
        for (int i = 1; i < snapshot.env_history_count; i++) {
            float value = snapshot.env_history[i].temperature;
            min_temp = value < min_temp ? value : min_temp;
            max_temp = value > max_temp ? value : max_temp;
        }
        added = added && cJSON_AddNumberToObject(json, "history_min_temperature_c", min_temp)
                && cJSON_AddNumberToObject(json, "history_max_temperature_c", max_temp);
    }
    if (!added) {
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    *result = json;
    return ESP_OK;
}

static esp_err_t tool_weather(const cJSON *args, cJSON **result)
{
    if (!has_no_arguments(args) || !result) {
        return ESP_ERR_INVALID_ARG;
    }
    sys_state_snapshot_t snapshot;
    sys_state_get_snapshot(&snapshot);
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    bool added = cJSON_AddBoolToObject(json, "available", snapshot.weather_available);
    if (snapshot.weather_available) {
        added = added && cJSON_AddNumberToObject(json, "temperature_c", snapshot.weather_temperature)
                && cJSON_AddNumberToObject(json, "humidity_percent", snapshot.weather_humidity)
                && cJSON_AddStringToObject(json, "condition", snapshot.weather_condition)
                && cJSON_AddStringToObject(json, "wind", snapshot.weather_wind);
    }
    if (!added) {
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    *result = json;
    return ESP_OK;
}

static bool has_only_fields(const cJSON *args, const char *first, const char *second)
{
    if (!cJSON_IsObject(args)) return false;
    bool got_first = false;
    bool got_second = second == NULL;
    for (const cJSON *item = args->child; item; item = item->next) {
        if (!item->string) return false;
        if (strcmp(item->string, first) == 0 && !got_first) got_first = true;
        else if (second && strcmp(item->string, second) == 0 && !got_second) got_second = true;
        else return false;
    }
    return got_first && got_second;
}

static bool get_bounded_uint(const cJSON *args, const char *name, uint32_t max, uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(args, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < 1 || item->valuedouble > max
        || item->valuedouble != (double)item->valueint) return false;
    *out = (uint32_t)item->valueint;
    return true;
}

static esp_err_t tool_web_search(const cJSON *args, cJSON **result)
{
    const cJSON *query = cJSON_GetObjectItemCaseSensitive(args, "query");
    uint32_t max_results = 0;
    if (!result || !has_only_fields(args, "query", "max_results") || !cJSON_IsString(query)
        || !query->valuestring[0] || strlen(query->valuestring) > 192
        || !get_bounded_uint(args, "max_results", WEB_CLIENT_MAX_RESULTS, &max_results)) {
        return ESP_ERR_INVALID_ARG;
    }
    web_client_search_result_t *sources = heap_caps_calloc(max_results, sizeof(*sources),
                                                             MALLOC_CAP_SPIRAM);
    if (!sources) return ESP_ERR_NO_MEM;
    size_t count = 0;
    esp_err_t err = web_client_search(query->valuestring, (uint8_t)max_results, sources,
                                      max_results, &count);
    if (err != ESP_OK) {
        free(sources);
        return err;
    }
    cJSON *json = cJSON_CreateObject();
    cJSON *array = json ? cJSON_AddArrayToObject(json, "sources") : NULL;
    if (!array) {
        cJSON_Delete(json);
        free(sources);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < count; i++) {
        cJSON *source = cJSON_CreateObject();
        if (!source || !cJSON_AddStringToObject(source, "url", sources[i].url)
            || !cJSON_AddStringToObject(source, "title", sources[i].title)
            || !cJSON_AddStringToObject(source, "snippet", sources[i].snippet)) {
            cJSON_Delete(source);
            cJSON_Delete(json);
            free(sources);
            return ESP_ERR_NO_MEM;
        }
        if (!cJSON_AddItemToArray(array, source)) {
            cJSON_Delete(source);
            cJSON_Delete(json);
            free(sources);
            return ESP_ERR_NO_MEM;
        }
    }
    free(sources);
    *result = json;
    return ESP_OK;
}

static esp_err_t tool_web_extract(const cJSON *args, cJSON **result)
{
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(args, "url");
    uint32_t max_chars = 0;
    if (!result || !has_only_fields(args, "url", "max_chars") || !cJSON_IsString(url)
        || strlen(url->valuestring) >= WEB_CLIENT_URL_MAX
        || !get_bounded_uint(args, "max_chars", WEB_CLIENT_EXTRACT_MAX, &max_chars)) {
        return ESP_ERR_INVALID_ARG;
    }
    char *content = heap_caps_malloc(max_chars + 1, MALLOC_CAP_SPIRAM);
    if (!content) return ESP_ERR_NO_MEM;
    size_t length = 0;
    esp_err_t err = web_client_extract(url->valuestring, max_chars, content, max_chars + 1, &length);
    if (err != ESP_OK) {
        free(content);
        return err;
    }
    cJSON *json = cJSON_CreateObject();
    if (!json || !cJSON_AddStringToObject(json, "url", url->valuestring)
        || !cJSON_AddStringToObject(json, "content", content)
        || !cJSON_AddBoolToObject(json, "truncated", length == max_chars)) {
        cJSON_Delete(json);
        free(content);
        return ESP_ERR_NO_MEM;
    }
    free(content);
    *result = json;
    return ESP_OK;
}

static esp_err_t tool_load_skill(const cJSON *args, cJSON **result)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(args, "name");
    if (!result || !has_only_fields(args, "name", NULL) || !cJSON_IsString(name)
        || !name->valuestring[0]) return ESP_ERR_INVALID_ARG;
    char *content = heap_caps_malloc(SKILL_MANAGER_CONTENT_MAX + 1, MALLOC_CAP_SPIRAM);
    if (!content) return ESP_ERR_NO_MEM;
    esp_err_t err = skill_manager_load(name->valuestring, content, SKILL_MANAGER_CONTENT_MAX + 1);
    if (err != ESP_OK) {
        free(content);
        return err;
    }
    cJSON *json = cJSON_CreateObject();
    if (!json || !cJSON_AddStringToObject(json, "name", name->valuestring)
        || !cJSON_AddStringToObject(json, "instructions", content)) {
        cJSON_Delete(json);
        free(content);
        return ESP_ERR_NO_MEM;
    }
    free(content);
    *result = json;
    return ESP_OK;
}

static const llm_tool_def_t s_definitions[] = {
    {.name = "get_device_status", .description = "读取设备的联网、存储、电池和时间状态。",
     .parameters_json = "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {.name = "get_environment", .description = "读取当前室内温湿度及有限历史温度范围。",
     .parameters_json = "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {.name = "get_weather", .description = "读取已经缓存的天气，不会发起新的网络请求。",
     .parameters_json = "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {.name = "web_search", .description = "搜索公开网页并返回带URL的精简来源。",
     .parameters_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"maxLength\":192},\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":5}},\"required\":[\"query\",\"max_results\"],\"additionalProperties\":false}"},
    {.name = "web_extract", .description = "只抓取最近搜索返回的公开来源正文。正文为去标签文本，开头可能含导航噪音，max_chars建议填8192。",
     .parameters_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"maxLength\":255},\"max_chars\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8192}},\"required\":[\"url\",\"max_chars\"],\"additionalProperties\":false}"},
    {.name = "load_skill", .description = "按名称加载已校验的本地Skill指导文本。",
     .parameters_json = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"maxLength\":32}},\"required\":[\"name\"],\"additionalProperties\":false}"},
};

static const agent_tool_t s_tools[] = {
    {.name = "get_device_status", .execute = tool_device_status},
    {.name = "get_environment", .execute = tool_environment},
    {.name = "get_weather", .execute = tool_weather},
    {.name = "web_search", .execute = tool_web_search},
    {.name = "web_extract", .execute = tool_web_extract},
    {.name = "load_skill", .execute = tool_load_skill},
};

static const char *const s_snapshot_tools[] = {
    "get_device_status", "get_environment", "get_weather",
};
static const char *s_skill_tools[5];
static llm_tool_def_t s_active_definitions[AGENT_ACTIVE_TOOL_MAX];

const char *const *agent_tools_get_skill_allowed_tools(size_t *out_count)
{
    size_t count = 0;
    for (size_t i = 0; i < sizeof(s_snapshot_tools) / sizeof(s_snapshot_tools[0]); i++) {
        s_skill_tools[count++] = s_snapshot_tools[i];
    }
    if (web_client_is_available()) {
        s_skill_tools[count++] = "web_search";
        s_skill_tools[count++] = "web_extract";
    }
    if (out_count) *out_count = count;
    return s_skill_tools;
}

const llm_tool_def_t *agent_tools_get_definitions(int *out_count)
{
    int count = 0;
    for (int i = 0; i < 3; i++) s_active_definitions[count++] = s_definitions[i];
    if (web_client_is_available()) {
        s_active_definitions[count++] = s_definitions[3];
        s_active_definitions[count++] = s_definitions[4];
    }
    if (skill_manager_is_available()) s_active_definitions[count++] = s_definitions[5];
    int memory_count = 0;
    const llm_tool_def_t *memory_tools = agent_memory_tools_get_definitions(&memory_count);
    for (int i = 0; i < memory_count; i++) s_active_definitions[count++] = memory_tools[i];
    int scheduler_count = 0;
    const llm_tool_def_t *scheduler_tools = agent_scheduler_tools_get_definitions(&scheduler_count);
    for (int i = 0; i < scheduler_count; i++) s_active_definitions[count++] = scheduler_tools[i];
    if (out_count) *out_count = count;
    return s_active_definitions;
}

static const agent_tool_t *find_tool(const char *name)
{
    if ((strncmp(name, "web_", 4) == 0 && !web_client_is_available())
        || (strcmp(name, "load_skill") == 0 && !skill_manager_is_available())) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(s_tools) / sizeof(s_tools[0]); i++) {
        if (strcmp(name, s_tools[i].name) == 0) return &s_tools[i];
    }
    return NULL;
}

static void write_error(char *out, size_t out_len, const char *reason)
{
    (void)snprintf(out, out_len, "{\"error\":\"%s\"}", reason);
}

esp_err_t agent_tools_execute(const llm_tool_call_t *call, char *out, size_t out_len)
{
    if (!call || !out || out_len == 0 || call->name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (call->arguments_truncated) {
        write_error(out, out_len, "工具参数超出字节预算");
        return ESP_OK;
    }
    const agent_tool_t *tool = find_tool(call->name);
    bool memory_tool = agent_memory_tools_handles(call->name);
    bool scheduler_tool = agent_scheduler_tools_handles(call->name);
    if (!tool && !memory_tool && !scheduler_tool) {
        write_error(out, out_len, "工具未注册");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "执行工具: %s", call->name);
    const char *arguments = call->arguments[0] ? call->arguments : "{}";
    cJSON *args = cJSON_Parse(arguments);
    if (!args) {
        write_error(out, out_len, "参数不是合法 JSON");
        return ESP_OK;
    }
    cJSON *result = NULL;
    esp_err_t err = tool ? tool->execute(args, &result)
        : memory_tool ? agent_memory_tools_execute(call->name, args, &result)
        : agent_scheduler_tools_execute(call->name, args, &result);
    cJSON_Delete(args);
    if (err != ESP_OK || !result) {
        const char *reason = "工具执行失败";
        if (err == ESP_ERR_TIMEOUT) {
            reason = "网络请求超时";
        } else if (err == ESP_ERR_HTTP_CONNECT) {
            reason = "网络连接失败";
        } else if (err == ESP_ERR_NO_MEM) {
            reason = "系统资源不足";
        } else if (err == ESP_ERR_INVALID_STATE) {
            reason = "系统忙，请稍后重试";
        } else if (err == ESP_ERR_NOT_FOUND) {
            reason = "未找到相关信息";
        } else if (err == ESP_ERR_INVALID_SIZE) {
            reason = "结果超出大小限制";
        }
        write_error(out, out_len, reason);
        cJSON_Delete(result);
        return ESP_OK;
    }
    char *serialized = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    if (!serialized || strlen(serialized) >= out_len) {
        write_error(out, out_len, "工具结果超出预算");
        free(serialized);
        return ESP_OK;
    }
    strcpy(out, serialized);
    free(serialized);
    return ESP_OK;
}
