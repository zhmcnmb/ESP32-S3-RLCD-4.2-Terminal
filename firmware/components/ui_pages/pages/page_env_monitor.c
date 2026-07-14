#include <stdio.h>
#include <time.h>

#include "display_st7305.h"
#include "env_logger.h"
#include "esp_log.h"
#include "ui_pages.h"

/*
 * 环境趋势页: 展示 env_logger 内存环形缓冲里的温湿度历史折线图。
 * 和首页的区别: 首页(page_home)是瞬时读数，这里是过去1小时的走势曲线。
 * 数据完全来自服务层(env_logger)，本页不直接读传感器(规范第5节)。
 *
 * 布局(400×300):
 *   外框        2px描边，与首页风格统一
 *   标题栏      y=6..40    "环境趋势"(scale2) + 副标题/采样计数
 *   温度趋势图  y=64..156
 *   湿度趋势图  y=160..252
 *   底部提示    y=264
 */

static const char *TAG = "page_env_monitor";

#define CHART_REFRESH_TICKS 30 /* env_logger每60s才产生新样本，不必每秒重绘 */

static int s_tick = 0;

/* 折线图: 圆角框 + 左上角标签 + 右上角量程 + 自适应缩放折线。
 * n<2(采样不足)时只显示占位文案。 */
static void draw_chart(int x, int y, int w, int h, const char *label,
                       const env_logger_sample_t *samples, int n, bool is_temp)
{
    st7305_draw_round_rect(x, y, w, h, 6, true);
    st7305_draw_utf8(x + 10, y + 8, 1, label, true);

    if (n < 2) {
        const char *msg = "采样中…";
        st7305_draw_utf8(x + (w - st7305_utf8_width(1, msg)) / 2, y + h / 2, 1, msg, true);
        return;
    }

    const int pad_top = 26, pad_bottom = 8, pad_side = 10;
    int plot_x = x + pad_side;
    int plot_w = w - 2 * pad_side;
    int plot_y = y + pad_top;
    int plot_h = h - pad_top - pad_bottom;

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

    char range[32];
    snprintf(range, sizeof(range), "%.1f-%.1f", min_v, max_v);
    st7305_draw_utf8(x + w - 10 - st7305_utf8_width(1, range), y + 8, 1, range, true);

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
}

static void draw(void)
{
    st7305_clear(false);
    st7305_draw_rect(2, 2, 396, 296, 1, true); /* 外框，与首页风格统一 */

    env_logger_sample_t samples[ENV_LOGGER_HISTORY_LEN];
    int n = env_logger_get_history(samples, ENV_LOGGER_HISTORY_LEN);

    st7305_draw_utf8(20, 6, 2, "环境趋势", true);

    /* 副标题说明和首页的区别: 首页是瞬时读数，这里是历史走势 */
    st7305_draw_utf8(20, 40, 1, "近1小时走势", true);
    char count_buf[24];
    snprintf(count_buf, sizeof(count_buf), "共%lu条", (unsigned long)env_logger_sample_count());
    st7305_draw_utf8(400 - 20 - st7305_utf8_width(1, count_buf), 40, 1, count_buf, true);
    st7305_fill_rect(20, 58, 360, 1, true);

    draw_chart(20, 64, 360, 92, "温度 C", samples, n, true);
    draw_chart(20, 160, 360, 92, "湿度 %", samples, n, false);

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
