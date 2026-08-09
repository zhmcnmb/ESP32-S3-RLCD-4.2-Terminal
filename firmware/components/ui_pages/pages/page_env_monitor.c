#include <stdio.h>
#include <time.h>
#include <string.h>

#include "display_st7305.h"
#include "esp_log.h"
#include "sys_state.h"
#include "ui_pages.h"
#include "ui_v2.h"

/*
 * 环境趋势页：两条趋势卡片，当前读数并入各自卡片头部，折线保留一小时时间方向。
 * 页面只读取 sys_state 快照(env_history)，不直接依赖 env_logger(规范第6节)。
 */

static const char *TAG = "page_env_monitor";

#define CHART_REFRESH_TICKS 30 /* env_logger每60s才产生新样本，不必每秒重绘 */

static int s_tick = 0;

static void draw_time_label(int x, int y, time_t timestamp)
{
    struct tm t;
    char text[8];
    localtime_r(&timestamp, &t);
    snprintf(text, sizeof(text), "%02d:%02d", t.tm_hour, t.tm_min);
    st7305_draw_text(x, y, 1, text, true);
}

/* 反白标签提供趋势带的固定锚点，当前值紧随其后而不另占一块区域。 */
static int draw_band_badge(int y, const char *label)
{
    int label_w = st7305_utf8_width(1, label);
    st7305_fill_rect(32, y + 8, label_w + 16, 18, true);
    st7305_draw_utf8(40, y + 9, 1, label, false);
    return 32 + label_w + 16;
}

static void draw_band_header(int y, int badge_end,
                             const sys_state_env_sample_t *sample, bool is_temp)
{
    char value[16];
    if (is_temp) {
        snprintf(value, sizeof(value), "%.1f", sample->temperature);
    } else {
        snprintf(value, sizeof(value), "%.0f", sample->humidity);
    }

    int value_x = badge_end + 14;
    st7305_draw_text(value_x, y + 5, 3, value, true);
    int value_end = value_x + 24 * (int)strlen(value);
    if (is_temp) {
        ui_v2_draw_temp_unit(value_end + 6, y + 8);
    } else {
        st7305_draw_text(value_end + 6, y + 13, 2, "%", true);
    }
}

/* 趋势卡片把量程置于折线右侧，给高一些的折线区保留完整观察空间。 */
static void draw_trend_band(int y, const char *label,
                            const sys_state_env_sample_t *samples, int n, bool is_temp)
{
    ui_v2_draw_card(20, y, 360, 116);
    int badge_end = draw_band_badge(y, label);
    if (n < 2) {
        char msg[32];
        snprintf(msg, sizeof(msg), "正在积累 %d/2", n);
        st7305_draw_utf8(badge_end + 14, y + 9, 1, msg, true);
        return;
    }
    draw_band_header(y, badge_end, &samples[n - 1], is_temp);

    float min_v = is_temp ? samples[0].temperature : samples[0].humidity;
    float max_v = min_v;
    for (int i = 1; i < n; i++) {
        float value = is_temp ? samples[i].temperature : samples[i].humidity;
        if (value < min_v) min_v = value;
        if (value > max_v) max_v = value;
    }
    if (max_v - min_v < 0.5f) {
        max_v += 0.5f;
        min_v -= 0.5f;
    }

    char range[16];
    snprintf(range, sizeof(range), "%.1f", max_v);
    st7305_draw_text(368 - 8 * (int)strlen(range), y + 34, 1, range, true);
    snprintf(range, sizeof(range), "%.1f", min_v);
    st7305_draw_text(368 - 8 * (int)strlen(range), y + 82, 1, range, true);

    const int plot_x = 40, plot_w = 288, plot_y = y + 34, plot_h = 56;
    for (int px = plot_x; px < plot_x + plot_w; px += 4) {
        st7305_set_pixel(px, plot_y + plot_h / 2, true); /* 中值参考虚线 */
    }
    int prev_x = plot_x;
    int prev_y = plot_y;
    for (int i = 0; i < n; i++) {
        float value = is_temp ? samples[i].temperature : samples[i].humidity;
        int x = plot_x + i * (plot_w - 1) / (n - 1);
        int py = plot_y + plot_h - 1 -
                 (int)((value - min_v) / (max_v - min_v) * (plot_h - 1));
        if (i > 0) {
            st7305_draw_line(prev_x, prev_y, x, py, true);
            st7305_draw_line(prev_x, prev_y + 1, x, py + 1, true); /* 2px笔画 */
        }
        prev_x = x;
        prev_y = py;
    }
    st7305_fill_rect(prev_x - 2, prev_y - 2, 5, 5, true);
    for (int px = plot_x; px < plot_x + plot_w; px += 4) {
        st7305_set_pixel(px, plot_y + plot_h, true);
    }
    draw_time_label(plot_x, plot_y + plot_h + 6, samples[0].timestamp);
    draw_time_label(plot_x + plot_w - 40, plot_y + plot_h + 6, samples[n - 1].timestamp);
}

static void draw(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);

    st7305_clear(false);
    ui_v2_draw_header("环境趋势", &snap);

    int n = snap.env_history_count;
    draw_trend_band(40, "温度", snap.env_history, n, true);
    draw_trend_band(164, "湿度", snap.env_history, n, false);

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

static const ui_page_t s_page = {
    .name = "env_monitor",
    .on_enter = on_enter,
    .on_exit = NULL,
    .on_tick = on_tick,
    .on_key = NULL,
};

const ui_page_t *page_env_monitor_get(void)
{
    return &s_page;
}
