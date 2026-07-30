/*
 * llm_request.c - LLM 统一请求编码。
 * 保证工具结果回填前，助手原始 tool_calls 会按 OpenAI 兼容格式一并回传。
 */
#include "llm_provider_internal.h"

#include <string.h>

#include "cJSON.h"

static esp_err_t add_assistant_tool_calls(cJSON *message,
                                          const llm_tool_call_t *calls, int count)
{
    if (!calls || count <= 0 || count > LLM_MAX_TOOL_CALLS) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *array = cJSON_AddArrayToObject(message, "tool_calls");
    if (!array) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < count; i++) {
        if (calls[i].id[0] == '\0' || calls[i].name[0] == '\0') {
            return ESP_ERR_INVALID_ARG;
        }
        const char *arguments = calls[i].arguments[0] ? calls[i].arguments : "{}";
        cJSON *call = cJSON_CreateObject();
        if (!call) {
            return ESP_ERR_NO_MEM;
        }
        cJSON *function = cJSON_AddObjectToObject(call, "function");
        if (!function) {
            cJSON_Delete(call);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(call, "id", calls[i].id);
        cJSON_AddStringToObject(call, "type", "function");
        cJSON_AddStringToObject(function, "name", calls[i].name);
        cJSON_AddStringToObject(function, "arguments", arguments);
        if (!cJSON_AddItemToArray(array, call)) {
            cJSON_Delete(call);
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static esp_err_t add_messages_to_array(cJSON *messages_json,
                                       const llm_message_t *messages, int message_count)
{
    for (int i = 0; i < message_count; i++) {
        cJSON *message = cJSON_CreateObject();
        if (!message) {
            return ESP_ERR_NO_MEM;
        }
        switch (messages[i].role) {
        case AGENT_ROLE_SYSTEM:
            cJSON_AddStringToObject(message, "role", "system");
            break;
        case AGENT_ROLE_USER:
            cJSON_AddStringToObject(message, "role", "user");
            break;
        case AGENT_ROLE_ASSISTANT:
            cJSON_AddStringToObject(message, "role", "assistant");
            break;
        case AGENT_ROLE_TOOL:
            cJSON_AddStringToObject(message, "role", "tool");
            if (messages[i].tool_call_id) {
                cJSON_AddStringToObject(message, "tool_call_id", messages[i].tool_call_id);
            }
            break;
        default:
            cJSON_Delete(message);
            return ESP_ERR_INVALID_ARG;
        }
        if (messages[i].content) {
            cJSON_AddStringToObject(message, "content", messages[i].content);
        } else {
            cJSON_AddNullToObject(message, "content");
        }
        if (messages[i].tool_call_count > 0) {
            if (messages[i].role != AGENT_ROLE_ASSISTANT) {
                cJSON_Delete(message);
                return ESP_ERR_INVALID_ARG;
            }
            esp_err_t err = add_assistant_tool_calls(message, messages[i].tool_calls,
                                                     messages[i].tool_call_count);
            if (err != ESP_OK) {
                cJSON_Delete(message);
                return err;
            }
        }
        cJSON_AddItemToArray(messages_json, message);
    }
    return ESP_OK;
}

static esp_err_t add_tools_to_array(cJSON *tools_json,
                                    const llm_tool_def_t *tools, int tool_count)
{
    for (int i = 0; i < tool_count; i++) {
        if (!tools[i].name || !tools[i].description) {
            return ESP_ERR_INVALID_ARG;
        }
        cJSON *tool = cJSON_CreateObject();
        if (!tool) {
            return ESP_ERR_NO_MEM;
        }
        cJSON *function = cJSON_AddObjectToObject(tool, "function");
        if (!function) {
            cJSON_Delete(tool);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(tool, "type", "function");
        cJSON_AddStringToObject(function, "name", tools[i].name);
        cJSON_AddStringToObject(function, "description", tools[i].description);
        if (tools[i].parameters_json) {
            cJSON *parameters = cJSON_Parse(tools[i].parameters_json);
            if (!parameters) {
                cJSON_Delete(tool);
                return ESP_ERR_INVALID_ARG;
            }
            cJSON_AddItemToObject(function, "parameters", parameters);
        }
        cJSON_AddItemToArray(tools_json, tool);
    }
    return ESP_OK;
}

esp_err_t llm_provider_build_request_body(const char *model,
                                          const llm_message_t *messages, int message_count,
                                          const llm_tool_def_t *tools, int tool_count,
                                          char **out_body, size_t *out_len)
{
    if (!model || !messages || message_count <= 0 || !out_body || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *messages_json = cJSON_AddArrayToObject(root, "messages");
    if (!root || !messages_json) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddBoolToObject(root, "stream", true);
    cJSON_AddBoolToObject(root, "enable_thinking", false);
    esp_err_t err = add_messages_to_array(messages_json, messages, message_count);
    if (err == ESP_OK && tools && tool_count > 0) {
        cJSON *tools_json = cJSON_AddArrayToObject(root, "tools");
        err = tools_json ? add_tools_to_array(tools_json, tools, tool_count) : ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return err;
    }
    *out_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!*out_body) {
        return ESP_ERR_NO_MEM;
    }
    *out_len = strlen(*out_body);
    return ESP_OK;
}
