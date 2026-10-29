#include <stdio.h>
#include <time.h>

#include "display_st7305.h"
#include "esp_log.h"
#include "sys_state.h"
#include "ui_pages.h"
#include "ui_v2.h"

static const char *TAG = "page_codex";

/* 本页只依赖用量数据本身: 用 updated_at + available 判重绘, 不用全局 version,
 * 否则环境/电池/语音任一变化都会把整屏重刷一遍。 */
static time_t s_last_updated;
static bool s_last_available;

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

/* 单个指标：小标签 + 大数字 + 注脚。数值按底边对齐（y=82），保证不同字号也成一条线。 */
static void draw_metric(int x, const char *label, const char *value, int scale,
                        const char *caption)
{
    st7305_draw_text(x, 52, 1, label, true);
    st7305_draw_text(x, 90 - 8 * scale, scale, value, true);
    st7305_draw_text(x, 100, 1, caption, true);
}

/* 顶部统计区：TODAY 用大号(scale3)做主视觉，CACHE/OUTPUT 用 scale2 做次级。
 * 不用卡片，只靠字号层级和一条分隔线分组。 */
static void draw_stat_cards(const sys_state_codex_usage_t *usage)
{
    char total[16], input[12], cached[12], output[12], hit_text[32], caption[24];
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
    draw_metric(28, "TODAY", total, 3, caption);
    snprintf(caption, sizeof(caption), "CACHED %s", cached);
    draw_metric(176, "CACHE", hit_text, 2, caption);
    snprintf(caption, sizeof(caption), "REQ %lu", (unsigned long)usage->requests);
    draw_metric(280, "OUTPUT", output, 2, caption);

    st7305_draw_line(20, 120, 380, 120, true);
}

/* 日额度整行：圆角额度条 + 已用/上限右对齐；limit 缺失时整行不画。 */
static void draw_day_quota(int y, float cost, float limit)
{
    char left[16], value[20];
    if (limit <= 0) return;
    float pct = cost / limit * 100;
    if (pct > 999) pct = 999;
    snprintf(left, sizeof(left), pct > 0.0f && pct < 1.0f ? "DAY  %.1f%%" : "DAY  %.0f%%", pct);
    if (cost >= 100.0f) snprintf(value, sizeof(value), "$%.0f/$%.0f", cost, limit);
    else snprintf(value, sizeof(value), "$%.1f/$%.0f", cost, limit);
    st7305_draw_text(32, y + 2, 1, left, true);
    st7305_draw_round_rect(112, y, 170, 12, 4, true);
    int bar_width = (int)(pct > 100 ? 162 : pct * 1.62f);
    if (bar_width > 0) st7305_fill_rect(114, y + 3, bar_width, 6, true);
    st7305_draw_text(372 - st7305_utf8_width(1, value), y + 2, 1, value, true);
}

static void format_history_cost(float value, char out[8])
{
    if (value >= 100.0f) snprintf(out, 8, "$%.0f", value);
    else if (value >= 10.0f) snprintf(out, 8, "$%.1f", value);
    else snprintf(out, 8, "$%.2f", value);
}

/* 设备本地日期(yyyymmdd); 时间未同步返回 0, 由调用方回退到"首行即今日"的旧行为。 */
static uint32_t today_ymd(const sys_state_snapshot_t *snap)
{
    struct tm local;
    if (!snap->time_valid || snap->now == 0 || localtime_r(&snap->now, &local) == NULL) {
        return 0;
    }
    return (uint32_t)(local.tm_year + 1900) * 10000U + (uint32_t)(local.tm_mon + 1) * 100U +
           (uint32_t)local.tm_mday;
}

/* 近 5 天消耗表：日期降序，今天的整行反白；不画表头分隔线，靠留白分组。 */
static void draw_days(const sys_state_codex_usage_t *usage, uint32_t today)
{
    st7305_draw_text(32, 174, 1, "5 DAYS", true);
    for (int i = 0; i < usage->history_count && i < 5; i++) {
        const sys_state_codex_day_t *day = &usage->history[i];
        int y = 192 + 17 * i;
        char date[6], cost[8], tokens[12];
        format_history_cost(day->cost, cost);
        format_tokens(day->tokens, tokens, sizeof(tokens));
        snprintf(date, sizeof(date), "%02lu/%02lu",
                 (unsigned long)(day->date / 100U % 100U),
                 (unsigned long)(day->date % 100U));
        bool is_today = today ? (day->date == today) : (i == 0);
        bool ink = !is_today;
        if (is_today) st7305_fill_rect(28, y - 2, 176, 12, true);
        st7305_draw_text(38, y, 1, date, ink);
        st7305_draw_text(152 - st7305_utf8_width(1, cost), y, 1, cost, ink);
        st7305_draw_text(202 - st7305_utf8_width(1, tokens), y, 1, tokens, ink);
    }
}

/* 模型榜：名字 + 右对齐 token；不画占比条，靠右对齐的数字比较大小。 */
static void draw_models(const sys_state_codex_usage_t *usage)
{
    st7305_draw_text(218, 174, 1, "TOP MODELS (TODAY)", true);
    if (usage->model_count == 0) {
        st7305_draw_text(218, 196, 1, "NO USAGE", true);
        return;
    }
    for (int i = 0; i < usage->model_count && i < SYS_CODEX_MODEL_LIST_LEN; i++) {
        const sys_state_codex_model_t *model = &usage->models[i];
        int y = 192 + 17 * i;
        char name[16], tokens[12];
        /* 桥接按 token 降序下发, 固件保序; 仅按 token 展示, 费用看额度卡。 */
        snprintf(name, sizeof(name), "%.13s", model->name);
        format_big_tokens(model->tokens, tokens, sizeof(tokens));
        st7305_draw_text(218, y, 1, name, true);
        st7305_draw_text(372 - st7305_utf8_width(1, tokens), y, 1, tokens, true);
    }
}

static void draw_usage(const sys_state_snapshot_t *snap)
{
    const sys_state_codex_usage_t *usage = &snap->codex_usage;
    draw_stat_cards(usage);

    /* 只保留日额度：中转的 weekly_limit_usd 一直是 null，周额度行已按需求下线。
     * 卡高 34、内容高 12，起始 y = 126 + (34-12)/2 = 137，内容在卡内垂直居中。 */
    ui_v2_draw_card(20, 126, 360, 34);
    draw_day_quota(137, usage->daily_cost, usage->daily_limit);

    ui_v2_draw_card(20, 166, 360, 114);
    draw_days(usage, today_ymd(snap));
    draw_models(usage);
}

/* 底部摸鱼阅读行：桥接每 10 分钟换一句，随用量包下发；无内容时不占视觉。 */
static void draw_novel(const sys_state_codex_usage_t *usage)
{
    if (!usage->novel_line[0]) return;
    int x = (400 - st7305_utf8_width(1, usage->novel_line)) / 2;
    st7305_draw_utf8(x < 0 ? 0 : x, 288, 1, usage->novel_line, true);
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
        draw_usage(snap);
        draw_novel(&snap->codex_usage);
    } else {
        ui_v2_draw_empty_state(132, snap->wifi_connected ? "WAIT" : "OFFLINE", NULL);
    }

    esp_err_t err = st7305_flush();
    if (err != ESP_OK) ESP_LOGW(TAG, "flush failed: %s", esp_err_to_name(err));
    else {
        s_last_updated = snap->codex_usage.updated_at;
        s_last_available = snap->codex_usage.available;
    }
}

static void on_enter(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);
    /* 先进页画当前快照，同时请求立即拉一次，避免等满整个轮询周期 */
    sys_state_request_codex_refresh();
    draw(&snap);
}

static void on_tick(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);
    if (snap.codex_usage.updated_at != s_last_updated ||
        snap.codex_usage.available != s_last_available) {
        draw(&snap);
    }
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







