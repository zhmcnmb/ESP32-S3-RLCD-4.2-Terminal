/* agent_scheduler_tools.c - 提醒调度的受限 Agent 工具 Adapter。 */
#include "agent_runtime_internal.h"

#include <stdint.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "scheduler.h"

static bool s_allow_set;
static bool s_allow_cancel;

static const llm_tool_def_t s_definitions[] = {
    {.name = "set_reminder", .description = "按用户明确要求创建一次或周期提醒。",
     .parameters_json = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"maxLength\":95},\"minutes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1440},\"repeat\":{\"type\":\"boolean\"}},\"required\":[\"text\",\"minutes\",\"repeat\"],\"additionalProperties\":false}"},
    {.name = "cancel_reminder", .description = "按用户明确要求取消指定编号的提醒。",
     .parameters_json = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\",\"minimum\":1}},\"required\":[\"id\"],\"additionalProperties\":false}"},
    {.name = "list_reminders", .description = "列出当前提醒及距触发的剩余分钟。",
     .parameters_json = "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
};

static bool contains_term(const char *text, const char *const terms[], size_t count)
{
    if (!text) return false;
    for (size_t i = 0; i < count; i++) {
        if (strstr(text, terms[i])) return true;
    }
    return false;
}

/* 否定取消不能误解为取消已有提醒，避免模型获得不必要的副作用能力。 */
static bool user_requests_cancel(const char *text)
{
    static const char *const nouns[] = {"提醒", "闹钟", "倒计时", "定时"};
    static const char *const direct[] = {"别提醒", "不用提醒", "不再提醒"};
    static const char *const verbs[] = {"取消", "删除", "移除", "关闭", "停止"};
    static const char *const negations[] = {
        "不要取消", "别取消", "不取消", "不用取消", "不要删除", "别删除", "不删除",
        "不用删除", "不要移除", "别移除", "不移除", "不用移除", "不要关闭", "别关闭",
        "不关闭", "不用关闭", "不要停止", "别停止", "不停止", "不用停止",
    };
    if (contains_term(text, negations, sizeof(negations) / sizeof(negations[0]))) return false;
    return contains_term(text, direct, sizeof(direct) / sizeof(direct[0]))
           || (contains_term(text, nouns, sizeof(nouns) / sizeof(nouns[0]))
               && contains_term(text, verbs, sizeof(verbs) / sizeof(verbs[0])));
}

/* 创建和取消混在同一句时只暴露取消工具，优先避免误建任务。 */
static bool user_requests_set(const char *text)
{
    static const char *const nouns[] = {"提醒", "闹钟", "倒计时", "定时"};
    static const char *const direct[] = {"提醒我", "帮我提醒", "给我提醒", "后叫我"};
    static const char *const verbs[] = {"设置", "设定", "设个", "设提醒", "添加", "创建", "定个"};
    static const char *const negations[] = {
        "不要设置", "别设置", "不设置", "不用设置", "不要设定", "别设定", "不设定",
        "不用设定", "不要添加", "别添加", "不添加", "不用添加", "不要创建", "别创建",
        "不创建", "不用创建", "不要提醒我", "别提醒我", "不提醒我", "不用提醒我",
    };
    if (user_requests_cancel(text)
        || contains_term(text, negations, sizeof(negations) / sizeof(negations[0]))) {
        return false;
    }
    return contains_term(text, direct, sizeof(direct) / sizeof(direct[0]))
           || (contains_term(text, nouns, sizeof(nouns) / sizeof(nouns[0]))
               && contains_term(text, verbs, sizeof(verbs) / sizeof(verbs[0])));
}

void agent_scheduler_tools_prepare(const char *user_text)
{
    s_allow_cancel = user_requests_cancel(user_text);
    s_allow_set = user_requests_set(user_text);
}

static esp_err_t tool_set(const cJSON *args, cJSON **out)
{
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(args, "text");
    const cJSON *repeat = cJSON_GetObjectItemCaseSensitive(args, "repeat");
    uint32_t minutes = 0;
    const char *required[] = {"text", "minutes", "repeat"};
    if (!s_allow_set || !out || !agent_args_validate(args, required, 3, NULL, 0)
        || !cJSON_IsString(text) || !text->valuestring[0]
        || strlen(text->valuestring) >= SCHEDULER_TEXT_MAX || !cJSON_IsBool(repeat)
        || !agent_args_parse_uint(args, "minutes", 1, 1440, &minutes)) return ESP_ERR_INVALID_ARG;
    cJSON *result = cJSON_CreateObject();
    cJSON *id_item = result ? cJSON_AddNumberToObject(result, "id", 0) : NULL;
    if (!id_item || !cJSON_AddNumberToObject(result, "fire_in_minutes", minutes)) {
        cJSON_Delete(result);
        return ESP_ERR_NO_MEM;
    }
    bool is_repeat = cJSON_IsTrue(repeat);
    uint32_t id = 0;
    esp_err_t err = scheduler_add(is_repeat ? SCHED_INTERVAL : SCHED_ONCE, minutes * 60,
                                  is_repeat ? minutes * 60 : 0, text->valuestring, &id);
    if (err != ESP_OK) {
        cJSON_Delete(result);
        return err;
    }
    cJSON_SetNumberValue(id_item, id);
    *out = result;
    return ESP_OK;
}

static esp_err_t tool_cancel(const cJSON *args, cJSON **out)
{
    uint32_t id = 0;
    const char *required[] = {"id"};
    if (!s_allow_cancel || !out || !agent_args_validate(args, required, 1, NULL, 0)
        || !agent_args_parse_uint(args, "id", 1, UINT32_MAX, &id)) return ESP_ERR_INVALID_ARG;
    cJSON *result = cJSON_CreateObject();
    cJSON *cancelled = result ? cJSON_AddBoolToObject(result, "cancelled", false) : NULL;
    if (!cancelled) {
        cJSON_Delete(result);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = scheduler_cancel(id);
    if (err != ESP_OK) {
        cJSON_Delete(result);
        return err;
    }
    cJSON_SetBoolValue(cancelled, true);
    *out = result;
    return ESP_OK;
}

static esp_err_t task_to_json(const scheduler_task_info_t *task, time_t now, cJSON *tasks)
{
    cJSON *json = cJSON_CreateObject();
    time_t remaining = task->fire_at > now ? task->fire_at - now : 0;
    if (!json || !cJSON_AddNumberToObject(json, "id", task->id)
        || !cJSON_AddStringToObject(json, "text", task->text)
        || !cJSON_AddNumberToObject(json, "remaining_minutes", (remaining + 59) / 60)
        || !cJSON_AddBoolToObject(json, "repeat", task->kind == SCHED_INTERVAL)
        || !cJSON_AddItemToArray(tasks, json)) {
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t tool_list(const cJSON *args, cJSON **out)
{
    scheduler_task_info_t items[SCHEDULER_TASK_MAX];
    size_t count = 0;
    if (!out || !agent_args_validate(args, NULL, 0, NULL, 0)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = scheduler_list(items, SCHEDULER_TASK_MAX, &count);
    if (err != ESP_OK) return err;
    cJSON *json = cJSON_CreateObject();
    cJSON *tasks = json ? cJSON_AddArrayToObject(json, "reminders") : NULL;
    if (!tasks) {
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    time_t now = time(NULL);
    for (size_t i = 0; i < count; i++) {
        err = task_to_json(&items[i], now, tasks);
        if (err != ESP_OK) {
            cJSON_Delete(json);
            return err;
        }
    }
    *out = json;
    return ESP_OK;
}

const llm_tool_def_t *agent_scheduler_tools_get_definitions(int *out_count)
{
    static llm_tool_def_t active[3];
    int count = 0;
    if (scheduler_is_available()) {
        active[count++] = s_definitions[2];
        if (s_allow_set) active[count++] = s_definitions[0];
        if (s_allow_cancel) active[count++] = s_definitions[1];
    }
    if (out_count) *out_count = count;
    return active;
}

bool agent_scheduler_tools_handles(const char *name)
{
    if (!scheduler_is_available() || !name) return false;
    if (strcmp(name, "list_reminders") == 0) return true;
    if (strcmp(name, "set_reminder") == 0) return s_allow_set;
    return strcmp(name, "cancel_reminder") == 0 && s_allow_cancel;
}

esp_err_t agent_scheduler_tools_execute(const char *name, const cJSON *args, cJSON **out)
{
    if (!agent_scheduler_tools_handles(name)) return ESP_ERR_NOT_FOUND;
    if (strcmp(name, "set_reminder") == 0) return tool_set(args, out);
    if (strcmp(name, "cancel_reminder") == 0) return tool_cancel(args, out);
    return tool_list(args, out);
}
