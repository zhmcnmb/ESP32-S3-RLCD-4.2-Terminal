#include <stdio.h>
#include <string.h>
#include <time.h>

#include "display_st7305.h"
#include "esp_log.h"
#include "sys_state.h"
#include "ui_pages.h"
#include "ui_v2.h"

/*
 * Claude用量页(预算仪表)：百分比是核心，离散块进度条与重置时间为解释信息。
 * 只读取 sys_state 快照，不直接轮询桥接服务(规范第6节)。
 *
 * 布局(400×300，UI V2共享页头/页脚见 ui_v2.h):
 *   页头        y=0..34    "CLAUDE用量" + SD/WiFi/电池图标
 *   新鲜度/旧数据徽标  y=44
 *   5H窗口      y=70..102
 *   7D窗口      y=150..182
 *   页脚        y=266..    KEY/VOICE/BOOT提示
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

/* 8段离散块进度条，符合单色点阵语言；用量>=80%额外画"!"标记，不依赖颜色 */
static void draw_segments(int x, int y, int w, int h, float fraction, bool warn)
{
    const int segs = 8;
    const int gap = 3;
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
        st7305_draw_text(x + w + 10, y + 4, 2, "!", true);
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
    st7305_draw_text(70, y + 2, 3, pct, true);

    int pw = 8 * 3 * (int)strlen(pct);
    draw_segments(70 + pw + 24, y + 4, 170, 16, used, used >= 0.8f);

    char remain[32];
    format_remaining(resets_at, now, remain, sizeof(remain));
    st7305_draw_utf8(20, y + 32, 1, remain, true);
}

static void draw(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);

    st7305_clear(false);
    ui_v2_draw_header("CLAUDE用量", &snap);

    bool has_data = snap.quota_updated_at != 0;
    bool stale = has_data && !snap.quota_available;

    char sub[32];
    if (!has_data) {
        snprintf(sub, sizeof(sub), "首次数据获取中");
    } else if (stale) {
        snprintf(sub, sizeof(sub), "旧数据");
    } else {
        long age = (long)(snap.now - snap.quota_updated_at);
        snprintf(sub, sizeof(sub), "更新%ld秒前", age < 0 ? 0 : age);
    }
    st7305_draw_utf8(400 - 20 - st7305_utf8_width(1, sub), 44, 1, sub, true);

    if (has_data) {
        draw_window(70, "5H", snap.quota_used_5h, snap.quota_resets_5h, snap.now);
        draw_window(150, "7D", snap.quota_used_7d, snap.quota_resets_7d, snap.now);
    } else {
        ui_v2_draw_empty_state(120, "正在获取", "首次连接桥接服务中");
    }

    ui_v2_draw_footer("刷新");

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
    if (evt == UI_KEY_ACTION) {
        draw();
        return true;
    }
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
