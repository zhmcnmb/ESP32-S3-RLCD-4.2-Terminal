/* agent_text.c - 不设固定上限的 PSRAM 文本缓冲。 */
#include "agent_runtime_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#define AGENT_TEXT_INITIAL_CAP 512
#define AGENT_TEXT_MAX (16 * 1024)

void agent_text_init(agent_text_t *text)
{
    if (text) {
        memset(text, 0, sizeof(*text));
    }
}

void agent_text_reset(agent_text_t *text)
{
    if (!text) {
        return;
    }
    text->len = 0;
    text->oom = false;
    if (text->data) {
        text->data[0] = '\0';
    }
}

static esp_err_t ensure_capacity(agent_text_t *text, size_t need)
{
    if (need == SIZE_MAX) {
        text->oom = true;
        return ESP_ERR_NO_MEM;
    }
    size_t required = need + 1;
    if (required > AGENT_TEXT_MAX) {
        text->oom = true;
        return ESP_ERR_NO_MEM;
    }
    if (text->cap >= required) {
        return ESP_OK;
    }
    size_t cap = text->cap ? text->cap : AGENT_TEXT_INITIAL_CAP;
    while (cap < required) {
        if (cap > SIZE_MAX / 2) {
            cap = required;
            break;
        }
        cap *= 2;
    }
    char *data = heap_caps_realloc(text->data, cap, MALLOC_CAP_SPIRAM);
    if (!data) {
        text->oom = true;
        return ESP_ERR_NO_MEM;
    }
    text->data = data;
    text->cap = cap;
    return ESP_OK;
}

esp_err_t agent_text_append(agent_text_t *text, const char *data, size_t len)
{
    if (!text || (!data && len != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (text->oom) {
        return ESP_ERR_NO_MEM;
    }
    if (len > SIZE_MAX - text->len) {
        text->oom = true;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ensure_capacity(text, text->len + len);
    if (err != ESP_OK) {
        return err;
    }
    if (len > 0) {
        memcpy(text->data + text->len, data, len);
    }
    text->len += len;
    text->data[text->len] = '\0';
    return ESP_OK;
}

const char *agent_text_c_str(const agent_text_t *text)
{
    return (text && text->data) ? text->data : "";
}

size_t agent_text_len(const agent_text_t *text)
{
    return text ? text->len : 0;
}

bool agent_text_oom(const agent_text_t *text)
{
    return text && text->oom;
}
