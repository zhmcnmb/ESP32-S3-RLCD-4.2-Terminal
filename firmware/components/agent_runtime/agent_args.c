/* agent_args.c - 工具参数校验共享 helper。
 * agent_memory_tools / agent_scheduler_tools 各持一份 has_fields/parse_uint
 * 的重复实现（签名微差、语义漂移风险），收拢为单一实现。 */
#include "agent_runtime_internal.h"

#include <string.h>

bool agent_args_validate(const cJSON *args, const char *const required[], int required_count,
                         const char *const optional[], int optional_count)
{
    if (!cJSON_IsObject(args)) return false;
    if (required_count == 0 && optional_count == 0) {
        return args->child == NULL;
    }
    for (const cJSON *item = args->child; item; item = item->next) {
        if (!item->string) return false;
        /* 重复键拒绝：不可信模型输入可能用双键混淆校验与取值 */
        for (const cJSON *prev = args->child; prev != item; prev = prev->next) {
            if (prev->string && strcmp(prev->string, item->string) == 0) {
                return false;
            }
        }
        bool matched = false;
        for (int i = 0; i < required_count; i++) {
            if (strcmp(item->string, required[i]) == 0) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            for (int i = 0; i < optional_count; i++) {
                if (strcmp(item->string, optional[i]) == 0) {
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) return false;
    }
    for (int i = 0; i < required_count; i++) {
        bool found = false;
        for (const cJSON *item = args->child; item; item = item->next) {
            if (item->string && strcmp(item->string, required[i]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

bool agent_args_parse_uint(const cJSON *args, const char *name, uint32_t min,
                           uint32_t max, uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(args, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < min || item->valuedouble > max
        || item->valuedouble != (double)(uint32_t)item->valuedouble) return false;
    *out = (uint32_t)item->valuedouble;
    return true;
}
