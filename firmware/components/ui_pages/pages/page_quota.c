#include <stdio.h>
#include <string.h>
#include <time.h>

#include "display_st7305.h"
#include "esp_log.h"
#include "sys_state.h"
#include "ui_pages.h"
#include "ui_v2.h"

/*
 * Claude用量页：百分比是核心，离散块进度条与重置时间为解释信息。
 * 只读取 sys_state 快照；Mac bridge 离线时本页独立降级(规范第6/8节)。
 *
 * 布局(400×300):
 *   页头        y=0..34    "CLAUDE用量" + SD/WiFi/电池图标
 *   异常徽标    y=44       仅旧/离线
 *   5H窗口      y=70..102
 *   7D窗口      y=150..182
 */

static const char *TAG = "page_quota";

#define REFRESH_TICKS 10 /* 客户端后台60s轮询一次，页面每10s重绘跟上缓存变化和倒计时 */

static int s_tick = 0;

/* 反白短标签("5H"/"7D")，形成稳定识别锚点(规范第9节) */
static void draw_badge(int x, int y, const char *label)
{
    const int w = 40, h = 24;
    st7305_fill_rect(x, y, w, h, true);
    int tw = 8 * 2 * (int)strlen(label);
    st7305_draw_text(x + (w - tw) / 2, y + (h - 16) / 2, 2, label, false);
}

/* 10段离散块让每一格直观对应10%用量；高用量仅用符号提示，不依赖颜色。 */
static void draw_segments(int x, int y, int w, int h, float fraction, bool warn)
{
    const int segs = 10;
    const int gap = 4;
    int seg_w = (w - gap * (segs - 1)) / segs;
    int filled = (int)(fraction * segs + 0.5f);
    if (filled > segs) {
        filled = segs;
    }
    for (int i = 0; i < segs; i++) {
        int sx = x + i * (seg_w + gap);
        if (i < filled) {
            st7305_fill_rect(sx, y, seg_w, h, true);
        } else {
            st7305_draw_rect(sx, y, seg_w, h, 1, true);
        }
    }
    if (warn) {
        st7305_draw_text(x + w + 12, y - 5, 3, "!", true);
    }
}

/* 剩余时间格式化: >=1天显示"X天Y小时"，否则"X小时Y分" */
static void format_remaining(time_t resets_at, time_t now, char *out, size_t out_len)
{
    long remain = (long)(resets_at - now);
    if (remain < 0) {
        remain = 0;
    }
    long days = remain / 86400;
    long hours = (remain % 86400) / 3600;
    long mins = (remain % 3600) / 60;
    if (days > 0) {
        snprintf(out, out_len, "%ld天%ld小时后重置", days, hours);
    } else {
        snprintf(out, out_len, "%ld小时%ld分后重置", hours, mins);
    }
}

static void draw_window(int y, const char *label, float used, time_t resets_at, time_t now)
{
    draw_badge(20, y, label);

    char pct[8];
    snprintf(pct, sizeof(pct), "%.0f%%", used * 100);
    st7305_draw_text(76, y - 4, 5, pct, true);

    char remain[32];
    format_remaining(resets_at, now, remain, sizeof(remain));
    st7305_draw_utf8(380 - st7305_utf8_width(1, remain), y + 8, 1, remain, true);

    draw_segments(20, y + 50, 316, 14, used, used >= 0.8f);
}

static void draw(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);

    st7305_clear(false);
    ui_v2_draw_header("CLAUDE用量", &snap);

    bool has_data = snap.quota_updated_at != 0;
    bool stale = has_data && !snap.quota_available;

    ui_v2_draw_status_badge(stale ? "旧" : (!has_data && !snap.wifi_connected ? "离线" : NULL));

    if (has_data) {
        draw_window(60, "5H", snap.quota_used_5h, snap.quota_resets_5h, snap.now);
        ui_v2_draw_dots_divider(157);
        draw_window(184, "7D", snap.quota_used_7d, snap.quota_resets_7d, snap.now);
    } else {
        /* Mac bridge 是本页唯一依赖；离线时独立降级，不提实现细节 */
        ui_v2_draw_empty_state(120, snap.wifi_connected ? "等待数据" : "离线", NULL);
    }

    esp_err_t err = st7305_flush();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "flush失败: %s", esp_err_to_name(err));
    }
}

static void on_enter(void)
{
    s_tick = 0;
    draw();
}

static void on_tick(void)
{
    if (++s_tick >= REFRESH_TICKS) {
        s_tick = 0;
        draw();
    }
}

static bool on_key(ui_key_event_t evt)
{
    (void)evt;
    return false;
}

static const ui_page_t s_page = {
    .name = "quota",
    .on_enter = on_enter,
    .on_exit = NULL,
    .on_tick = on_tick,
    .on_key = on_key,
};

const ui_page_t *page_quota_get(void)
{
    return &s_page;
}
