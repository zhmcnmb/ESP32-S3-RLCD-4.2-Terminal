#include <stdio.h>
#include <time.h>

#include "display_st7305.h"
#include "esp_log.h"
#include "sys_state.h"
#include "ui_pages.h"
#include "ui_v2.h"

static const char *TAG = "page_codex";

static uint32_t s_last_version;

static void format_tokens(uint64_t value, char *out, size_t out_len)
{
    if (value >= 1000000000000ULL) {
        snprintf(out, out_len, "%.2fT", (double)value / 1000000000000.0);
    } else if (value >= 1000000000ULL) {
        snprintf(out, out_len, "%.2fB", (double)value / 1000000000.0);
    } else if (value >= 1000000ULL) {
        snprintf(out, out_len, "%.2fM", (double)value / 1000000.0);
    } else if (value >= 1000ULL) {
        snprintf(out, out_len, "%.1fK", (double)value / 1000.0);
    } else {
        snprintf(out, out_len, "%llu", (unsigned long long)value);
    }
}

/* 大字数值：scale2 下卡内最多 6 字符，百万位以上压缩为 x.xM / x.xxB。 */
static void format_big_tokens(uint64_t value, char *out, size_t out_len)
{
    if (value >= 1000000000ULL) {
        snprintf(out, out_len, "%.2fB", (double)value / 1000000000.0);
    } else if (value >= 1000000ULL) {
        snprintf(out, out_len, "%.1fM", (double)value / 1000000.0);
    } else {
        format_tokens(value, out, out_len);
    }
}

/* Token Monitor 风格统计小卡：小标签 + 大数字 + 小注。 */
static void draw_stat_card(int x, const char *label, const char *value, const char *caption)
{
    ui_v2_draw_card(x, 40, 112, 56);
    st7305_draw_text(x + 8, 48, 1, label, true);
    st7305_draw_text(x + 8, 60, 2, value, true);
    st7305_draw_text(x + 8, 82, 1, caption, true);
}

static void draw_stat_cards(const sys_state_codex_usage_t *usage)
{
    char total[16], input[12], cached[12], output[12], hit_text[32], caption[20];
    uint64_t all_tokens = usage->input_tokens;
    if (UINT64_MAX - all_tokens < usage->cached_tokens ||
        UINT64_MAX - (all_tokens + usage->cached_tokens) < usage->output_tokens) {
        all_tokens = UINT64_MAX;
    } else {
        all_tokens += usage->cached_tokens + usage->output_tokens;
    }
    format_big_tokens(all_tokens, total, sizeof(total));
    format_tokens(usage->input_tokens, input, sizeof(input));
    format_tokens(usage->cached_tokens, cached, sizeof(cached));
    format_tokens(usage->output_tokens, output, sizeof(output));
    double cache_base = (double)usage->input_tokens + (double)usage->cached_tokens;
    double hit = cache_base > 0 ? (double)usage->cached_tokens / cache_base * 100 : 0;
    int hit_tenths = hit > 100.0 ? 1000 : (int)(hit * 10.0 + 0.5);
    if (hit_tenths < 0) hit_tenths = 0;
    snprintf(hit_text, sizeof(hit_text), "%d.%d%%", hit_tenths / 10, hit_tenths % 10);

    snprintf(caption, sizeof(caption), "IN %s", input);
    draw_stat_card(20, "TODAY TOKENS", total, caption);
    snprintf(caption, sizeof(caption), "CACHE %s", cached);
    draw_stat_card(144, "CACHE HIT", hit_text, caption);
    snprintf(caption, sizeof(caption), "REQ %lu", (unsigned long)usage->requests);
    draw_stat_card(268, "OUTPUT", output, caption);
}

/* 额度整行：标签+占比 | 进度条 | 已用/上限右对齐；limit<=0 整行不画。 */
static void draw_quota(int y, const char *label, float cost, float limit)
{
    char left[16], value[20];
    if (limit <= 0) return;
    float pct = cost / limit * 100;
    if (pct > 999) pct = 999;
    snprintf(left, sizeof(left), "%s  %.0f%%", label, pct);
    if (cost >= 100.0f) snprintf(value, sizeof(value), "$%.0f/$%.0f", cost, limit);
    else snprintf(value, sizeof(value), "$%.1f/$%.0f", cost, limit);
    st7305_draw_text(36, y, 1, left, true);
    st7305_draw_rect(120, y, 160, 8, 1, true);
    int bar_width = (int)(pct > 100 ? 156 : pct * 1.56f);
    if (bar_width > 0) st7305_fill_rect(122, y + 2, bar_width, 4, true);
    st7305_draw_text(372 - st7305_utf8_width(1, value), y, 1, value, true);
}

static void format_history_cost(float value, char out[8])
{
    if (value >= 100.0f) snprintf(out, 8, "$%.0f", value);
    else if (value >= 10.0f) snprintf(out, 8, "$%.1f", value);
    else snprintf(out, 8, "$%.2f", value);
}

/* 近7天消耗表，history 日期降序且首行为今日，行首标 >。 */
static void draw_days(const sys_state_codex_usage_t *usage)
{
    st7305_draw_text(36, 160, 1, "7 DAYS  $ / TOK", true);
    for (int i = 0; i < usage->history_count && i < SYS_CODEX_HISTORY_LEN; i++) {
        const sys_state_codex_day_t *day = &usage->history[i];
        int y = 172 + 14 * i;
        char date[6], cost[8], tokens[12];
        format_history_cost(day->cost, cost);
        format_tokens(day->tokens, tokens, sizeof(tokens));
        snprintf(date, sizeof(date), "%02lu/%02lu",
                 (unsigned long)(day->date / 100U % 100U),
                 (unsigned long)(day->date % 100U));
        if (i == 0) st7305_draw_text(27, y, 1, ">", true);
        st7305_draw_text(38, y, 1, date, true);
        st7305_draw_text(88, y, 1, cost, true);
        st7305_draw_text(202 - st7305_utf8_width(1, tokens), y, 1, tokens, true);
    }
}

static void draw_models(const sys_state_codex_usage_t *usage)
{
    st7305_draw_text(218, 160, 1, "TOP MODELS (24H)", true);
    if (usage->model_count == 0) {
        st7305_draw_text(218, 176, 1, "NO USAGE", true);
        return;
    }
    for (int i = 0; i < usage->model_count && i < SYS_CODEX_MODEL_LIST_LEN; i++) {
        const sys_state_codex_model_t *model = &usage->models[i];
        char name[16], tokens[12];
        /* 桥接按 token 降序排列, cost 同为 0 时固件保序; 仅按 token 展示, 费用看额度卡。 */
        snprintf(name, sizeof(name), "%.13s", model->name);
        format_big_tokens(model->tokens, tokens, sizeof(tokens));
        st7305_draw_text(218, 176 + 16 * i, 1, name, true);
        st7305_draw_text(372 - st7305_utf8_width(1, tokens), 176 + 16 * i, 1, tokens, true);
    }
}

static void draw_usage(const sys_state_codex_usage_t *usage)
{
    draw_stat_cards(usage);

    ui_v2_draw_card(20, 104, 360, 40);
    draw_quota(110, "DAY", usage->daily_cost, usage->daily_limit);
    draw_quota(126, "WEEK", usage->weekly_usage, usage->weekly_limit);

    ui_v2_draw_card(20, 152, 360, 118);
    st7305_draw_line(210, 160, 210, 262, true);
    draw_days(usage);
    draw_models(usage);
}

/* 底部摸鱼阅读行：桥接按 60s 一行轮换，随用量包下发；无内容时不占视觉。 */
static void draw_novel(const sys_state_codex_usage_t *usage)
{
    if (!usage->novel_line[0]) return;
    int x = (400 - st7305_utf8_width(1, usage->novel_line)) / 2;
    st7305_draw_utf8(x < 0 ? 0 : x, 280, 1, usage->novel_line, true);
}

static void draw(const sys_state_snapshot_t *snap)
{
    st7305_clear(false);
    ui_v2_draw_header("REECHI", snap);

    bool has_data = snap->codex_usage.updated_at != 0;
    const char *badge = has_data && !snap->codex_usage.available ? "OLD" :
                        (!has_data && !snap->wifi_connected ? "OFFLINE" : NULL);
    ui_v2_draw_status_badge(badge);
    if (has_data) {
        draw_usage(&snap->codex_usage);
        draw_novel(&snap->codex_usage);
    } else {
        ui_v2_draw_empty_state(132, snap->wifi_connected ? "WAIT" : "OFFLINE", NULL);
    }

    esp_err_t err = st7305_flush();
    if (err != ESP_OK) ESP_LOGW(TAG, "flush failed: %s", esp_err_to_name(err));
    else s_last_version = snap->version;
}

static void on_enter(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);
    s_last_version = 0;
    draw(&snap);
}

static void on_tick(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);
    if (snap.version != s_last_version) draw(&snap);
}

static const ui_page_t s_page = {
    .name = "codex_usage",
    .on_enter = on_enter,
    .on_exit = NULL,
    .on_tick = on_tick,
    .on_key = NULL,
};

const ui_page_t *page_codex_usage_get(void)
{
    return &s_page;
}
