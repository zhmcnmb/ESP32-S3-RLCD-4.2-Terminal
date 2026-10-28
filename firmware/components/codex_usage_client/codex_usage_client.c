#include "codex_usage_client.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "cloud_transport.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sys_state.h"

static const char *TAG = "codex_usage";

#define POLL_INTERVAL_MS (60 * 1000)
#define POLL_INTERVAL_BATTERY_MS (60 * 1000)
/* 实测 7 天+6 模型约 3.7KB，已贴近 4KB；留 8KB 余量，超限再提高。 */
#define RESPONSE_SIZE 8192

typedef struct {
    char *data;
    size_t len;
    size_t capacity;
} response_buffer_t;

static char s_base_url[96];
static char s_auth_header[96];
static char *s_response;

static bool api_key_valid(const char *value)
{
    size_t n = value ? strlen(value) : 0;
    if (n < 20 || n >= sizeof(s_auth_header) - 8 || strncmp(value, "sk-", 3) != 0) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (value[i] <= 32 || value[i] > 126) return false;
    }
    return true;
}

static esp_err_t collect_chunk(const char *data, size_t len, void *ctx)
{
    response_buffer_t *buffer = ctx;
    if (len >= buffer->capacity - buffer->len) return ESP_ERR_INVALID_SIZE;
    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    buffer->data[buffer->len] = '\0';
    return ESP_OK;
}

static bool json_nonnegative(const cJSON *item, double *out)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || item->valuedouble < 0) {
        return false;
    }
    *out = item->valuedouble;
    return true;
}

static bool json_u64(const cJSON *obj, const char *key, uint64_t *out)
{
    const double max_exact_integer = 9007199254740991.0;
    double value;
    if (!json_nonnegative(cJSON_GetObjectItem(obj, key), &value) ||
        value > max_exact_integer || value != (double)(uint64_t)value) {
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

static bool json_optional_u64(const cJSON *obj, const char *key, uint64_t *out)
{
    if (!cJSON_GetObjectItem(obj, key)) {
        *out = 0;
        return true;
    }
    return json_u64(obj, key, out);
}

static bool json_cost(const cJSON *obj, const char *key, float *out)
{
    double value;
    if (!json_nonnegative(cJSON_GetObjectItem(obj, key), &value) || value > FLT_MAX) {
        return false;
    }
    *out = (float)value;
    return true;
}

static bool read_counts(const cJSON *obj, uint64_t *input, uint64_t *cached,
                        uint64_t *output, uint32_t *requests, float *cost)
{
    uint64_t cache_read, cache_create, cache_write, req;
    /* 线上 today/model_stats 用 cache_creation_tokens，daily 行用 cache_write_tokens，两个键都要认。 */
    if (!json_u64(obj, "input_tokens", input) || !json_u64(obj, "output_tokens", output) ||
        !json_optional_u64(obj, "cache_read_tokens", &cache_read) ||
        !json_optional_u64(obj, "cache_creation_tokens", &cache_create) ||
        !json_optional_u64(obj, "cache_write_tokens", &cache_write) ||
        !json_u64(obj, "requests", &req) || req > UINT32_MAX ||
        !json_cost(obj, "cost", cost) ||
        UINT64_MAX - cache_read < cache_create ||
        UINT64_MAX - (cache_read + cache_create) < cache_write) {
        return false;
    }
    *cached = cache_read + cache_create + cache_write;
    *requests = (uint32_t)req;
    return true;
}

static uint64_t sum_tokens(uint64_t input, uint64_t cached, uint64_t output)
{
    if (UINT64_MAX - input < cached || UINT64_MAX - (input + cached) < output) {
        return UINT64_MAX;
    }
    return input + cached + output;
}

static bool parse_ymd(const char *text, uint32_t *out)
{
    unsigned year, month, day;
    if (!text || sscanf(text, "%u-%u-%u", &year, &month, &day) != 3 ||
        year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31) {
        return false;
    }
    *out = year * 10000U + month * 100U + day;
    return true;
}

static void history_upsert(sys_state_codex_usage_t *usage, uint32_t date, float cost,
                           uint64_t tokens)
{
    int pos = 0;
    while (pos < usage->history_count && usage->history[pos].date > date) pos++;
    if (pos < usage->history_count && usage->history[pos].date == date) {
        usage->history[pos].cost = cost;
        usage->history[pos].tokens = tokens;
        return;
    }
    if (pos >= SYS_CODEX_HISTORY_LEN) return;
    int count = usage->history_count < SYS_CODEX_HISTORY_LEN ?
                usage->history_count + 1 : SYS_CODEX_HISTORY_LEN;
    for (int i = count - 1; i > pos; i--) usage->history[i] = usage->history[i - 1];
    usage->history[pos] = (sys_state_codex_day_t){.date = date, .cost = cost, .tokens = tokens};
    usage->history_count = count;
}

static bool add_model(const cJSON *model, sys_state_codex_usage_t *usage)
{
    cJSON *name = cJSON_GetObjectItem(model, "model");
    uint64_t input, cached, output, tokens;
    uint32_t requests;
    float cost;
    if (!cJSON_IsString(name) ||
        !read_counts(model, &input, &cached, &output, &requests, &cost)) {
        return false;
    }
    if (!json_u64(model, "total_tokens", &tokens)) tokens = sum_tokens(input, cached, output);

    int pos = 0;
    while (pos < usage->model_count && cost <= usage->models[pos].cost) pos++;
    if (pos >= SYS_CODEX_MODEL_LIST_LEN) return true;
    int count = usage->model_count < SYS_CODEX_MODEL_LIST_LEN ?
                usage->model_count + 1 : SYS_CODEX_MODEL_LIST_LEN;
    for (int i = count - 1; i > pos; i--) usage->models[i] = usage->models[i - 1];
    usage->models[pos] = (sys_state_codex_model_t){.cost = cost, .tokens = tokens};
    snprintf(usage->models[pos].name, sizeof(usage->models[pos].name), "%s", name->valuestring);
    usage->model_count = count;
    return true;
}

static bool parse_history(const cJSON *days, sys_state_codex_usage_t *usage)
{
    if (!cJSON_IsArray(days)) return false;
    const cJSON *day = NULL;
    cJSON_ArrayForEach(day, days) {
        cJSON *date = cJSON_GetObjectItem(day, "date");
        uint32_t ymd;
        uint64_t tokens, input, cached, output;
        uint32_t requests;
        float cost;
        if (!cJSON_IsString(date) || !parse_ymd(date->valuestring, &ymd) ||
            !read_counts(day, &input, &cached, &output, &requests, &cost)) {
            return false;
        }
        if (!json_u64(day, "total_tokens", &tokens)) tokens = sum_tokens(input, cached, output);
        history_upsert(usage, ymd, cost, tokens);
    }
    return true;
}

static esp_err_t parse_response(sys_state_codex_usage_t *usage)
{
    cJSON *root = cJSON_Parse(s_response);
    cJSON *valid = root ? cJSON_GetObjectItem(root, "isValid") : NULL;
    cJSON *today = root ? cJSON_GetObjectItem(cJSON_GetObjectItem(root, "usage"), "today") : NULL;
    cJSON *models = root ? cJSON_GetObjectItem(root, "model_stats") : NULL;
    cJSON *days = root ? cJSON_GetObjectItem(root, "daily_usage") : NULL;
    cJSON *limits = root ? cJSON_GetObjectItem(root, "subscription") : NULL;
    bool ok = root && (!valid || cJSON_IsTrue(valid)) && cJSON_IsObject(today) &&
              cJSON_IsArray(models) &&
              read_counts(today, &usage->input_tokens, &usage->cached_tokens,
                          &usage->output_tokens, &usage->requests, &usage->daily_cost) &&
              parse_history(days, usage);
    if (ok && cJSON_IsObject(limits)) {
        ok = json_cost(limits, "daily_limit_usd", &usage->daily_limit);
        /* 周/月额度可缺省或为 null（线上 monthly_limit_usd 即 null），缺失不作为解析失败。 */
        if (ok) (void)json_cost(limits, "weekly_usage_usd", &usage->weekly_usage);
        if (ok) (void)json_cost(limits, "weekly_limit_usd", &usage->weekly_limit);
    }
    if (ok) {
        const cJSON *model = NULL;
        cJSON_ArrayForEach(model, models) {
            if (!add_model(model, usage)) {
                ok = false;
                break;
            }
        }
    }
    if (ok) {
        /* 摸鱼阅读行(可选)：桥接保证整字切割, 这里直接拷贝 */
        cJSON *novel = cJSON_GetObjectItem(root, "novel");
        cJSON *text = cJSON_IsObject(novel) ? cJSON_GetObjectItem(novel, "text") : NULL;
        if (cJSON_IsString(text) && text->valuestring[0]) {
            snprintf(usage->novel_line, sizeof(usage->novel_line), "%s", text->valuestring);
        }
    }
    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static bool usage_changed(const sys_state_codex_usage_t *next)
{
    /* 内容无变化不应用: 避免每次轮询都 bump 版本导致墨水屏高频重绘。
     * 双方均来自 {0} 初始化结构, 填充字节确定, 可整块比较; updated_at 仅时间戳, 不参与。 */
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);
    sys_state_codex_usage_t cur = snap.codex_usage;
    sys_state_codex_usage_t cand = *next;
    cur.updated_at = 0;
    cand.updated_at = 0;
    return memcmp(&cur, &cand, sizeof(cand)) != 0;
}

static esp_err_t poll_once(void)
{
    char url[160];
    int n = snprintf(url, sizeof(url),
                     "%s/v1/usage?days=7&timezone=Asia/Shanghai", s_base_url);
    if (n < 0 || (size_t)n >= sizeof(url)) return ESP_ERR_INVALID_SIZE;

    response_buffer_t buffer = {.data = s_response, .capacity = RESPONSE_SIZE};
    s_response[0] = '\0';
    int status = 0;
    const char *auth = s_auth_header[0] ? s_auth_header : NULL;
    esp_err_t err = cloud_transport_get_stream(url, auth ? "Authorization" : NULL, auth,
                                               collect_chunk, &buffer, NULL, &status);
    sys_state_codex_usage_t usage = {0};
    if (err == ESP_OK) err = parse_response(&usage);
    if (err != ESP_OK) {
        sys_state_mark_codex_usage_unavailable();
        ESP_LOGW(TAG, "usage update failed: %s, status=%d", esp_err_to_name(err), status);
        return err;
    }
    if (!usage_changed(&usage)) {
        return ESP_OK; /* 内容与当前一致, 不 bump 版本不触发重绘 */
    }
    usage.available = true;
    usage.updated_at = time(NULL);
    sys_state_apply_codex_usage(&usage);
    return ESP_OK;
}

static uint32_t poll_delay_ms(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);
    return (snap.battery_ok && !snap.battery_charging) ?
           POLL_INTERVAL_BATTERY_MS : POLL_INTERVAL_MS;
}

static void usage_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        poll_once();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(poll_delay_ms()));
    }
}

esp_err_t codex_usage_client_init(const char *base_url, const char *api_key)
{
    if (s_response) return ESP_OK;
    bool plain_http = base_url && strncmp(base_url, "http://", 7) == 0;
    /* http 仅用于局域网聚合桥, 不发送中转密钥; https 仍要求合法 sk- key。 */
    if (!base_url || (!plain_http && strncmp(base_url, "https://", 8) != 0) ||
        (!plain_http && !api_key_valid(api_key))) {
        return ESP_ERR_INVALID_ARG;
    }
    int n = snprintf(s_base_url, sizeof(s_base_url), "%s", base_url);
    if (n < 0 || (size_t)n >= sizeof(s_base_url)) return ESP_ERR_INVALID_SIZE;
    size_t len = strlen(s_base_url);
    while (len > 8 && s_base_url[len - 1] == '/') s_base_url[--len] = '\0';
    if (len > 10 && strcmp(s_base_url + len - 10, "/dashboard") == 0) {
        s_base_url[len - 10] = '\0';
    }
    if (plain_http) {
        s_auth_header[0] = '\0';
    } else {
        n = snprintf(s_auth_header, sizeof(s_auth_header), "Bearer %s", api_key);
        if (n < 0 || (size_t)n >= sizeof(s_auth_header)) return ESP_ERR_INVALID_SIZE;
    }

    s_response = heap_caps_malloc(RESPONSE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_response) return ESP_ERR_NO_MEM;
    BaseType_t ok = xTaskCreate(usage_task, "codex_usage_task", 8192, NULL, 3, NULL);
    if (ok != pdPASS) {
        heap_caps_free(s_response);
        s_response = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "started, refresh=%ds", POLL_INTERVAL_MS / 1000);
    /* 上游聚合桥(tools/usage_bridge.py默认30s)缓存周期小于拉取周期，每次拉取都拿到新数据 */
    return ESP_OK;
}
