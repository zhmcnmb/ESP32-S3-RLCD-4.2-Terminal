/* web_client.c - Jina Search/Reader 的受限云端 Adapter。 */
#include "web_client.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "cloud_transport.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "web_client";
static const char *SEARCH_URL = "https://s.jina.ai/";
static const char *READER_URL = "https://r.jina.ai/";

#define WEB_API_KEY_MAX 184
#define WEB_QUERY_MAX 192
#define WEB_RESPONSE_MAX (16 * 1024)

static char s_api_key[WEB_API_KEY_MAX + 1];
static bool s_initialized;
static char s_source_urls[WEB_CLIENT_MAX_RESULTS][WEB_CLIENT_URL_MAX];
static size_t s_source_count;

static void copy_utf8(char *out, size_t out_size, const char *text)
{
    if (!out || out_size == 0) return;
    size_t len = text ? strlen(text) : 0;
    if (len >= out_size) len = out_size - 1;
    while (len > 0 && (((unsigned char)text[len] & 0xc0) == 0x80)) len--;
    if (len > 0) memcpy(out, text, len);
    out[len] = '\0';
}

static bool valid_public_url(const char *url)
{
    if (!url) return false;
    size_t prefix = strncasecmp(url, "https://", 8) == 0 ? 8
                  : strncasecmp(url, "http://", 7) == 0 ? 7 : 0;
    if (prefix == 0) return false;
    const char *host = url + prefix;
    const char *end = strpbrk(host, "/?#");
    size_t host_len = end ? (size_t)(end - host) : strlen(host);
    if (host_len < 3 || host_len >= 254 || !isalpha((unsigned char)host[0])) return false;

    char normalized[254];
    bool has_dot = false;
    for (size_t i = 0; i < host_len; i++) {
        unsigned char c = (unsigned char)host[i];
        if (!(isalnum(c) || c == '.' || c == '-')) return false;
        normalized[i] = (char)tolower(c);
        has_dot |= c == '.';
    }
    normalized[host_len] = '\0';
    if (!has_dot || strcmp(normalized, "localhost") == 0) return false;
    size_t suffix = sizeof(".localhost") - 1;
    if (host_len >= suffix && strcmp(normalized + host_len - suffix, ".localhost") == 0) return false;
    suffix = sizeof(".local") - 1;
    return host_len < suffix || strcmp(normalized + host_len - suffix, ".local") != 0;
}

static bool is_search_source(const char *url)
{
    for (size_t i = 0; i < s_source_count; i++) {
        if (strcmp(url, s_source_urls[i]) == 0) return true;
    }
    return false;
}

static esp_err_t post_json(cJSON *request, cJSON **out)
{
    char *body = cJSON_PrintUnformatted(request);
    if (!body) return ESP_ERR_NO_MEM;
    char *response = heap_caps_calloc(1, WEB_RESPONSE_MAX, MALLOC_CAP_SPIRAM);
    if (!response) {
        free(body);
        return ESP_ERR_NO_MEM;
    }
    size_t response_len = 0;
    int status = 0;
    cloud_transport_json_request_t transport = {
        .url = cJSON_GetObjectItemCaseSensitive(request, "q") ? SEARCH_URL : READER_URL,
        .auth_header_value = s_api_key, .body = body, .body_len = strlen(body),
        .response = response, .response_size = WEB_RESPONSE_MAX,
    };
    esp_err_t err = cloud_transport_post_json(&transport, &response_len, &status);
    free(body);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Jina请求失败: %s status=%d", esp_err_to_name(err), status);
        free(response);
        return err;
    }
    *out = cJSON_ParseWithLength(response, response_len);
    free(response);
    return *out ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static const cJSON *response_array(const cJSON *root)
{
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "results");
    if (!cJSON_IsArray(array)) array = cJSON_GetObjectItemCaseSensitive(root, "data");
    return cJSON_IsArray(array) ? array : NULL;
}

static const char *string_field(const cJSON *object, const char *primary, const char *fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, primary);
    if (!cJSON_IsString(item) && fallback) item = cJSON_GetObjectItemCaseSensitive(object, fallback);
    return cJSON_IsString(item) ? item->valuestring : "";
}

esp_err_t web_client_init(const char *api_key)
{
    if (!api_key || !api_key[0] || strlen(api_key) > WEB_API_KEY_MAX) return ESP_ERR_INVALID_ARG;
    copy_utf8(s_api_key, sizeof(s_api_key), api_key);
    s_initialized = true;
    ESP_LOGI(TAG, "Jina Search/Reader 已配置");
    return ESP_OK;
}

bool web_client_is_available(void)
{
    return s_initialized;
}

esp_err_t web_client_search(const char *query, uint8_t max_results,
                            web_client_search_result_t *results, size_t result_capacity,
                            size_t *out_count)
{
    if (!s_initialized || !query || !query[0] || strlen(query) > WEB_QUERY_MAX || max_results == 0
        || max_results > WEB_CLIENT_MAX_RESULTS || !results || result_capacity < max_results || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(s_source_urls, 0, sizeof(s_source_urls));
    s_source_count = 0;
    cJSON *request = cJSON_CreateObject();
    if (!request) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(request, "q", query);
    cJSON_AddNumberToObject(request, "num", max_results);
    cJSON *root = NULL;
    esp_err_t err = post_json(request, &root);
    cJSON_Delete(request);
    if (err != ESP_OK) return err;

    size_t count = 0;
    const cJSON *array = response_array(root);
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        const char *url = string_field(item, "url", "link");
        if (count == max_results || !valid_public_url(url)) continue;
        copy_utf8(results[count].url, sizeof(results[count].url), url);
        copy_utf8(results[count].title, sizeof(results[count].title), string_field(item, "title", NULL));
        copy_utf8(results[count].snippet, sizeof(results[count].snippet),
                  string_field(item, "snippet", "description"));
        copy_utf8(s_source_urls[count], sizeof(s_source_urls[count]), url);
        count++;
        s_source_count++;
    }
    cJSON_Delete(root);
    *out_count = count;
    return count ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t web_client_extract(const char *url, size_t max_chars, char *out,
                             size_t out_size, size_t *out_len)
{
    if (!s_initialized || !valid_public_url(url) || !is_search_source(url)
        || max_chars == 0 || max_chars > WEB_CLIENT_EXTRACT_MAX
        || !out || out_size <= max_chars || !out_len) return ESP_ERR_INVALID_ARG;
    cJSON *request = cJSON_CreateObject();
    if (!request) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(request, "url", url);
    cJSON *root = NULL;
    esp_err_t err = post_json(request, &root);
    cJSON_Delete(request);
    if (err != ESP_OK) return err;

    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON *content = cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "content")
                                                 : cJSON_GetObjectItemCaseSensitive(root, "content");
    if (!cJSON_IsString(content)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t len = strlen(content->valuestring);
    if (len > max_chars) len = max_chars;
    while (len > 0 && (((unsigned char)content->valuestring[len] & 0xc0) == 0x80)) len--;
    memcpy(out, content->valuestring, len);
    out[len] = '\0';
    *out_len = len;
    cJSON_Delete(root);
    return ESP_OK;
}
