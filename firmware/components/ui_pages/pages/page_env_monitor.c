#include <stdio.h>
#include <time.h>

#include "display_st7305.h"
#include "esp_log.h"
#include "sys_state.h"
#include "ui_pages.h"
#include "ui_v2.h"

/*
 * 环境趋势页(趋势仪表)：先显示当前值，再显示两条共享时间方向的曲线。
 * 只读取 sys_state 快照(env_history)，不直接依赖 env_logger(规范第6节)。
 *
 * 布局(400×300，UI V2共享页头/页脚见 ui_v2.h):
 *   页头        y=0..34    "环境趋势" + SD/WiFi/电池图标
 *   采样计数    y=44       右对齐"共N条采样"
 *   当前值      y=64       温度/湿度当前读数
 *   温度趋势带  y=100..146
 *   湿度趋势带  y=176..222
 *   页脚        y=266..    KEY/VOICE/BOOT提示
 */

static const char *TAG = "page_env_monitor";

#define CHART_REFRESH_TICKS 30 /* env_logger每60s才产生新样本，不必每秒重绘 */

static int s_tick = 0;

/* 趋势带: 标签+范围一行，下方一条自适应缩放折线，底部居中"60分钟前到现在"。
 * 采样不足两条时保留同样高度，显示"正在积累"而不是空白大框(规范第8节) */
static void draw_trend_band(int y, const char *label,
                            const sys_state_env_sample_t *samples, int n, bool is_temp)
{
    st7305_draw_utf8(20, y, 1, label, true);

    if (n < 2) {
        char msg[40];
        snprintf(msg, sizeof(msg), "正在积累 %d/2", n);
        st7305_draw_utf8(120, y, 1, msg, true);
        return;
    }

    float min_v = is_temp ? samples[0].temperature : samples[0].humidity;
    float max_v = min_v;
    for (int i = 1; i < n; i++) {
        float v = is_temp ? samples[i].temperature : samples[i].humidity;
        if (v < min_v) min_v = v;
        if (v > max_v) max_v = v;
    }
    if (max_v - min_v < 0.5f) {
        /* 数值几乎不变时人为撑开量程，避免折线贴边看不出走势 */
        max_v += 0.5f;
        min_v -= 0.5f;
    }

    char range[16];
    snprintf(range, sizeof(range), "%.1f", min_v);
    st7305_draw_utf8(70, y, 1, range, true);
    snprintf(range, sizeof(range), "%.1f", max_v);
    st7305_draw_utf8(400 - 20 - st7305_utf8_width(1, range), y, 1, range, true);

    const int plot_x = 110, plot_w = 240, plot_y = y + 14, plot_h = 22;
    int prev_x = plot_x, prev_y = plot_y;
    for (int i = 0; i < n; i++) {
        float v = is_temp ? samples[i].temperature : samples[i].humidity;
        int px = plot_x + i * (plot_w - 1) / (n - 1);
        int py = plot_y + plot_h - 1 - (int)((v - min_v) / (max_v - min_v) * (plot_h - 1));
        if (i > 0) {
            st7305_draw_line(prev_x, prev_y, px, py, true);
        }
        prev_x = px;
        prev_y = py;
    }

    const char *caption = "60分钟前 到 现在";
    st7305_draw_utf8((400 - st7305_utf8_width(1, caption)) / 2, y + 40, 1, caption, true);
}

static void draw(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);

    st7305_clear(false);
    ui_v2_draw_header("环境趋势", &snap);

    char count_buf[24];
    snprintf(count_buf, sizeof(count_buf), "共%lu条采样", (unsigned long)snap.env_sample_total);
    st7305_draw_utf8(400 - 20 - st7305_utf8_width(1, count_buf), 44, 1, count_buf, true);

    int n = snap.env_history_count;
    if (n > 0) {
        const sys_state_env_sample_t *cur = &snap.env_history[n - 1];
        char line[48];
        snprintf(line, sizeof(line), "%.1f C 当前          %.0f %% 当前",
                 cur->temperature, cur->humidity);
        st7305_draw_utf8(20, 64, 1, line, true);
    }

    draw_trend_band(100, "温度", snap.env_history, n, true);
    draw_trend_band(176, "湿度", snap.env_history, n, false);

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
    if (++s_tick >= CHART_REFRESH_TICKS) {
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
    return false; /* NEXT交回ui_manager切页 */
}

static const ui_page_t s_page = {
    .name = "env_monitor",
    .on_enter = on_enter,
    .on_exit = NULL,
    .on_tick = on_tick,
    .on_key = on_key,
};

const ui_page_t *page_env_monitor_get(void)
{
    return &s_page;
}
