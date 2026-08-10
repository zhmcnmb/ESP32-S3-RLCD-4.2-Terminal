/* web_client.c - 免Key联网检索的受限 Adapter：
 * Bing(cn.bing.com) 搜索(GET，字符串匹配解析) + 白名单内URL直连抓取正文
 * (去script/style/标签后截断)。全程无需任何API Key。
 * 选型说明：DDG Lite 在目标网络被反爬拦截(202挑战页)，s.jina.ai 搜索强制要Key，
 * r.jina.ai 从设备TLS握手被重置；Bing是本网络实测可达且结果URL为直链的免Key搜索。 */
#include "web_client.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cloud_transport.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "web_client";
/* cn.bing.com 全球可达，避开 www.bing.com 的 302(get_stream 不跟随重定向) */
static const char *SEARCH_URL_BASE = "https://cn.bing.com/search?q=";
static const char *SEARCH_USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko)"
    " Chrome/126.0 Safari/537.36";

#define WEB_QUERY_MAX 192
#define WEB_SEARCH_HTML_MAX (128 * 1024) /* Bing整页约100KB，结果区在60~72KB处 */
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

/* 查询词 percent-encode：unreserved字符原样，其余%XX(中文UTF-8自然编码)。溢出返回0 */
static size_t url_encode_query(const char *query, char *out, size_t out_size)
{
    const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)query; *p; p++) {
        unsigned char c = *p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            if (o + 1 >= out_size) return 0;
            out[o++] = (char)c;
        } else {
            if (o + 3 >= out_size) return 0;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = '\0';
    return o;
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} html_sink_t;

/* 结果区在页面靠前位置，超出缓冲静默截断仍可用 */
static esp_err_t html_sink_cb(const char *data, size_t len, void *ctx)
{
    html_sink_t *sink = ctx;
    size_t room = sink->cap - 1 - sink->len;
    if (len > room) len = room;
    memcpy(sink->buf + sink->len, data, len);
    sink->len += len;
    sink->buf[sink->len] = '\0';
    return ESP_OK;
}

/* 展开常见/数值实体、去标签、压缩空白，供标题/摘要清洗 */
static void clean_html_text(const char *src, size_t src_len, char *out, size_t out_size)
{
    static const struct { const char *name; char ch; } ENTITIES[] = {
        { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' },
        { "&#39;", '\'' }, { "&nbsp;", ' ' }, { "&ensp;", ' ' }, { "&emsp;", ' ' },
        { "&middot;", ' ' },
    };
    if (!out || out_size == 0) return;
    size_t o = 0, i = 0;
    bool pending_space = false;
    while (i < src_len && o + 1 < out_size) {
        unsigned char c = (unsigned char)src[i];
        if (c == '<') {
            const char *gt = memchr(src + i, '>', src_len - i);
            i = gt ? (size_t)(gt - src) + 1 : src_len;
            continue;
        }
        if (c == '&') {
            size_t adv = 0;
            for (size_t e = 0; e < sizeof(ENTITIES) / sizeof(ENTITIES[0]); e++) {
                size_t n = strlen(ENTITIES[e].name);
                if (src_len - i >= n && strncmp(src + i, ENTITIES[e].name, n) == 0) {
                    c = (unsigned char)ENTITIES[e].ch;
                    adv = n;
                    break;
                }
            }
            if (adv == 0 && i + 2 < src_len && src[i + 1] == '#') {
                /* 数值实体 &#NN;：ASCII 输出，其余丢弃 */
                size_t j = i + 2;
                unsigned v = 0;
                while (j < src_len && isdigit((unsigned char)src[j]) && v < 512) {
                    v = v * 10 + (unsigned)(src[j] - '0');
                    j++;
                }
                if (j < src_len && src[j] == ';') {
                    adv = j + 1 - i;
                    c = v >= 0x20 && v < 0x7F ? (unsigned char)v : 0; /* 0=丢弃 */
                }
            }
            if (adv == 0) {
                /* 未知实体：界内找到';'则整体跳过，否则原样输出'&' */
                const char *semi = memchr(src + i, ';', src_len - i < 10 ? src_len - i : 10);
                if (semi) {
                    i = (size_t)(semi - src) + 1;
                    continue;
                }
                adv = 1;
            }
            i += adv;
            if (c == 0) continue;
        } else {
            i++;
        }
        if (isspace(c)) {
            pending_space = o > 0;
            continue;
        }
        if (pending_space) {
            out[o++] = ' ';
            pending_space = false;
        }
        out[o++] = (char)c;
    }
    out[o] = '\0';
}

/* 从一个 "<h2" 标记提取一条 Bing 结果；*next 一律推进到该锚点之后 */
static bool parse_result(const char *mark, const char **next,
                         web_client_search_result_t *out)
{
    const char *a_end = strstr(mark, "</a>");
    if (!a_end) {
        *next = mark + 3;
        return false;
    }
    const char *href = NULL;
    for (const char *q = mark; q + 6 <= a_end; q++) {
        if (strncmp(q, "href=\"", 6) == 0) {
            href = q + 6;
            break;
        }
    }
    const char *href_end = href ? memchr(href, '"', (size_t)(a_end - href)) : NULL;
    const char *a_gt = href_end ? memchr(href_end, '>', (size_t)(a_end - href_end)) : NULL;
    size_t url_len = href_end ? (size_t)(href_end - href) : 0;
    if (url_len == 0 || url_len >= sizeof(out->url) || !a_gt) {
        *next = a_end + 4;
        return false;
    }
    memcpy(out->url, href, url_len);
    out->url[url_len] = '\0';
    clean_html_text(a_gt + 1, (size_t)(a_end - a_gt - 1), out->title, sizeof(out->title));
    const char *snip = strstr(a_end, "b_lineclamp");
    if (snip) {
        const char *sgt = strchr(snip, '>');
        const char *p_end = sgt ? strstr(sgt, "</p>") : NULL;
        if (sgt && p_end) {
            clean_html_text(sgt + 1, (size_t)(p_end - sgt - 1), out->snippet, sizeof(out->snippet));
        }
    }
    *next = a_end + 4;
    /* 相关问题/站内链接是相对路径或 bing.com 自身，只保留外站直链 */
    return out->title[0] != '\0' && valid_public_url(out->url)
           && !strstr(out->url, "bing.com/");
}

static const char *find_nocase(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, needle, n) == 0) return p;
    }
    return NULL;
}

/* 删除 script/style/注释块(内容不是正文)，返回净化后长度 */
static size_t strip_noise_blocks(const char *in, char *out, size_t out_cap)
{
    size_t o = 0;
    while (*in && o + 1 < out_cap) {
        if (*in == '<') {
            const char *end = NULL;
            size_t close_len = 0;
            if (strncasecmp(in, "<script", 7) == 0) {
                end = find_nocase(in + 7, "</script");
                close_len = 9;
            } else if (strncasecmp(in, "<style", 6) == 0) {
                end = find_nocase(in + 6, "</style");
                close_len = 8;
            } else if (strncmp(in, "<!--", 4) == 0) {
                end = strstr(in + 4, "-->");
                close_len = 3;
            }
            if (end) {
                in = end + close_len;
                continue;
            }
        }
        out[o++] = *in++;
    }
    out[o] = '\0';
    return o;
}

esp_err_t web_client_init(void)
{
    s_initialized = true;
    ESP_LOGI(TAG, "联网检索已配置(免Key)");
    return ESP_OK;
}

void web_client_reset_sources(void)
{
    memset(s_source_urls, 0, sizeof(s_source_urls));
    s_source_count = 0;
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
    web_client_reset_sources();
    /* 完整搜索URL = 固定前缀 + percent-encode查询词 */
    char url[48 + WEB_QUERY_MAX * 3];
    strcpy(url, SEARCH_URL_BASE);
    if (url_encode_query(query, url + strlen(SEARCH_URL_BASE),
                         sizeof(url) - strlen(SEARCH_URL_BASE)) == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *html = heap_caps_calloc(1, WEB_SEARCH_HTML_MAX, MALLOC_CAP_SPIRAM);
    if (!html) return ESP_ERR_NO_MEM;
    html_sink_t sink = { .buf = html, .len = 0, .cap = WEB_SEARCH_HTML_MAX };
    int status = 0;
    esp_err_t err = cloud_transport_get_stream(url, "User-Agent", SEARCH_USER_AGENT,
                                               html_sink_cb, &sink, NULL, &status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "搜索请求失败: %s status=%d", esp_err_to_name(err), status);
        free(html);
        return err;
    }

    size_t count = 0;
    const char *p = html;
    while (count < max_results && (p = strstr(p, "<h2"))) {
        const char *next = p + 3;
        if (parse_result(p, &next, &results[count])) {
            copy_utf8(s_source_urls[count], sizeof(s_source_urls[count]), results[count].url);
            count++;
        }
        p = next;
    }
    free(html);
    s_source_count = count;
    ESP_LOGI(TAG, "搜索命中 %u 条: %s", (unsigned)count, query);
    *out_count = count;
    return count ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t web_client_extract(const char *url, size_t max_chars, char *out,
                             size_t out_size, size_t *out_len)
{
    if (!s_initialized || !valid_public_url(url) || !is_search_source(url)
        || max_chars == 0 || max_chars > WEB_CLIENT_EXTRACT_MAX
        || !out || out_size <= max_chars || !out_len) {
        ESP_LOGW(TAG, "抽取被拒绝(非搜索白名单或非法URL): %.80s", url ? url : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    /* 白名单内URL直连抓取；净化后截断到 max_chars(保持UTF-8边界) */
    char *html = heap_caps_calloc(1, WEB_SEARCH_HTML_MAX, MALLOC_CAP_SPIRAM);
    if (!html) return ESP_ERR_NO_MEM;
    html_sink_t sink = { .buf = html, .len = 0, .cap = WEB_SEARCH_HTML_MAX };
    int status = 0;
    esp_err_t err = cloud_transport_get_stream(url, "User-Agent", SEARCH_USER_AGENT,
                                               html_sink_cb, &sink, NULL, &status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "正文抓取失败: %s status=%d", esp_err_to_name(err), status);
        free(html);
        return err;
    }
    char *clean = heap_caps_calloc(1, WEB_SEARCH_HTML_MAX, MALLOC_CAP_SPIRAM);
    if (!clean) {
        free(html);
        return ESP_ERR_NO_MEM;
    }
    size_t clean_len = strip_noise_blocks(html, clean, WEB_SEARCH_HTML_MAX);
    free(html);
    clean_html_text(clean, clean_len, out, max_chars + 1);
    free(clean);
    size_t len = strlen(out);
    while (len > 0 && (((unsigned char)out[len] & 0xc0) == 0x80)) out[--len] = '\0';
    *out_len = len;
    return len ? ESP_OK : ESP_ERR_NOT_FOUND;
}
