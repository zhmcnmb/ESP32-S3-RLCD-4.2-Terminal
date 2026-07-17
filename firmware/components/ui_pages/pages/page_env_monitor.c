#include <stdio.h>
#include <time.h>
#include <string.h>

#include "display_st7305.h"
#include "esp_log.h"
#include "sys_state.h"
#include "ui_pages.h"
#include "ui_v2.h"

/*
 * 环境趋势页：上方给出当前温湿度，底部两条折线共享同一小时的时间方向。
 * 折线是数据本身，保留；页面框线、重复时间说明和空格对齐全部删除。
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

/* 趋势带只保留标签、量程与真实折线；时间端点说明同一条数据的覆盖范围。 */
static void draw_trend_band(int y, const char *label,
                            const sys_state_env_sample_t *samples, int n, bool is_temp)
{
    int label_w = st7305_utf8_width(1, label);
    st7305_fill_rect(20, y, label_w + 16, 18, true);
    st7305_draw_utf8(28, y + 1, 1, label, false);

    if (n < 2) {
        char msg[32];
        snprintf(msg, sizeof(msg), "正在积累 %d/2", n);
        st7305_draw_utf8(88, y + 1, 1, msg, true);
        return;
    }

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
    snprintf(range, sizeof(range), "%.1f", min_v);
    st7305_draw_text(88, y + 4, 1, range, true);
    snprintf(range, sizeof(range), "%.1f", max_v);
    st7305_draw_text(380 - 8 * (int)strlen(range), y + 4, 1, range, true);

    const int plot_x = 20, plot_w = 360, plot_y = y + 28, plot_h = 30;
    int prev_x = plot_x;
    int prev_y = plot_y;
    for (int i = 0; i < n; i++) {
        float value = is_temp ? samples[i].temperature : samples[i].humidity;
        int x = plot_x + i * (plot_w - 1) / (n - 1);
        int py = plot_y + plot_h - 1 -
                 (int)((value - min_v) / (max_v - min_v) * (plot_h - 1));
        if (i > 0) {
            st7305_draw_line(prev_x, prev_y, x, py, true);
        }
        prev_x = x;
        prev_y = py;
    }
    st7305_fill_rect(prev_x - 2, prev_y - 2, 5, 5, true);
    draw_time_label(plot_x, y + 64, samples[0].timestamp);
    draw_time_label(380 - 40, y + 64, samples[n - 1].timestamp);
}

static void draw(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);

    st7305_clear(false);
    ui_v2_draw_header("环境趋势", &snap);

    int n = snap.env_history_count;
    if (n > 0) {
        const sys_state_env_sample_t *cur = &snap.env_history[n - 1];
        char temp[16], humidity[16];
        snprintf(temp, sizeof(temp), "%.1f", cur->temperature);
        snprintf(humidity, sizeof(humidity), "%.0f", cur->humidity);

        st7305_draw_utf8(20, 48, 1, "温度", true);
        st7305_draw_text(20, 68, 4, temp, true);
        st7305_draw_utf8(20 + st7305_utf8_width(4, temp) + 6, 84, 1, "C", true);
        st7305_draw_utf8(220, 48, 1, "湿度", true);
        st7305_draw_text(220, 68, 4, humidity, true);
        st7305_draw_utf8(220 + st7305_utf8_width(4, humidity) + 6, 84, 1, "%", true);
    }

    draw_trend_band(126, "温度", snap.env_history, n, true);
    draw_trend_band(210, "湿度", snap.env_history, n, false);

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
    (void)evt;
    return false;
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
