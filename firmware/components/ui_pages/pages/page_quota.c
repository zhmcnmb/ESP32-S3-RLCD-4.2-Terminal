#include <stdio.h>
#include <string.h>
#include <time.h>

#include "claude_quota_client.h"
#include "display_st7305.h"
#include "esp_log.h"
#include "ui_pages.h"

/*
 * Claude用量页: 展示Claude订阅5小时/7天用量窗口。数据经 claude_quota_client
 * 后台轮询Mac桥接服务(bridge/server.py)获取，权威数据非估算(见CLAUDE.md P4章节)。
 * 本页不直接发HTTP请求，只读服务层缓存(规范第5节)。
 *
 * 布局(400×300):
 *   外框 2px
 *   标题栏      y=6..40     "Claude用量" + 数据新鲜度/降级提示
 *   5小时窗口   y=64..152   进度条+百分比+重置倒计时
 *   7天窗口     y=160..248  同上
 *   底部提示    y=264
 */

static const char *TAG = "page_quota";

#define REFRESH_TICKS 10 /* 客户端后台60s轮询一次，页面每10s重绘跟上缓存变化和倒计时 */

static int s_tick = 0;

static void draw_bar(int x, int y, int w, int h, float fraction)
{
    if (fraction < 0) {
        fraction = 0;
    }
    if (fraction > 1) {
        fraction = 1;
    }
    st7305_draw_rect(x, y, w, h, 1, true);
    int fill = (int)((w - 4) * fraction);
    if (fill > 0) {
        st7305_fill_rect(x + 2, y + 2, fill, h - 4, true);
    }
}

/* 剩余时间格式化: >=1天显示"X天Y小时"，否则"X小时Y分" */
static void format_remaining(time_t resets_at, char *out, size_t out_len)
{
    long remain = (long)(resets_at - time(NULL));
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

static void draw_window(int x, int y, int w, int h, const char *label,
                        float used, time_t resets_at)
{
    st7305_draw_round_rect(x, y, w, h, 8, true);

    st7305_draw_utf8(x + 14, y + 12, 1, label, true);

    char pct[8];
    snprintf(pct, sizeof(pct), "%.0f%%", used * 100);
    int pw = 8 * 3 * (int)strlen(pct); /* ascii scale3 等宽 */
    st7305_draw_text(x + w - 14 - pw, y + 8, 3, pct, true);

    draw_bar(x + 14, y + 44, w - 28, 20, used);

    char remain[32];
    format_remaining(resets_at, remain, sizeof(remain));
    st7305_draw_utf8(x + 14, y + 68, 1, remain, true);
}

static void draw(void)
{
    st7305_clear(false);
    st7305_draw_rect(2, 2, 396, 296, 1, true);

    claude_quota_t q;
    bool has_data = claude_quota_client_get(&q);

    st7305_draw_utf8(20, 6, 2, "Claude用量", true);

    char sub[32];
    if (!has_data) {
        snprintf(sub, sizeof(sub), "等待首次获取...");
    } else if (!q.available) {
        snprintf(sub, sizeof(sub), "桥接服务不可达");
    } else {
        long age = (long)(time(NULL) - q.fetched_at);
        snprintf(sub, sizeof(sub), "数据于%ld秒前", age < 0 ? 0 : age);
    }
    st7305_draw_utf8(20, 40, 1, sub, true);
    st7305_fill_rect(20, 58, 360, 1, true);

    if (has_data && q.available) {
        draw_window(20, 64, 360, 88, "5小时窗口", q.used_5h, q.resets_5h);
        draw_window(20, 160, 360, 88, "7天窗口", q.used_7d, q.resets_7d);
    } else {
        st7305_draw_round_rect(20, 64, 360, 184, 8, true);
        const char *msg = has_data ? "无法连接桥接服务，稍后自动重试" : "首次数据获取中，请稍候";
        st7305_draw_utf8(20 + (360 - st7305_utf8_width(1, msg)) / 2, 152, 1, msg, true);
    }

    st7305_fill_rect(20, 256, 360, 1, true);
    const char *hint = "KEY 切换页面";
    st7305_draw_utf8((400 - st7305_utf8_width(1, hint)) / 2, 264, 1, hint, true);

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
