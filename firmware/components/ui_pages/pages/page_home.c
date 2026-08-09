#include <stdio.h>
#include <string.h>
#include <time.h>

#include "display_st7305.h"
#include "esp_log.h"
#include "sys_state.h"
#include "ui_icons.h"
#include "ui_pages.h"
#include "ui_v2.h"

/*
 * 时钟页：时间是主要视觉焦点，环境信息降为底部两列摘要。
 * 只读取 sys_state 快照，不直接访问传感器/电池/网络/SD(规范第6节)。
 *
 * 布局(400×300):
 *   页头      y=0..34    状态图标(无重复标题)
 *   时间卡片  y=44..176  HH:MM 超大字(冒号每秒闪烁) + 秒 + 日期
 *   指标卡片  y=188..264 温湿度两列摘要
 */

static const char *TAG = "page_home";

static const char *WEEKDAY_CN[] = { "星期日", "星期一", "星期二", "星期三",
                                    "星期四", "星期五", "星期六" };

static void draw_time_section(const struct tm *t, const sys_state_snapshot_t *snap)
{
    char buf[16];

    ui_v2_draw_card(20, 44, 360, 132);

    /* 冒号每秒闪烁：空格与冒号同宽，布局不抖动 */
    snprintf(buf, sizeof(buf), "%02d%s%02d", t->tm_hour,
             (t->tm_sec % 2) ? " " : ":", t->tm_min);
    st7305_draw_text(40, 56, 8, buf, true);

    snprintf(buf, sizeof(buf), "%02d", t->tm_sec);
    st7305_draw_text(340, 148, 2, buf, true);

    char date_buf[48];
    snprintf(date_buf, sizeof(date_buf), "%d月%d日 %s",
             t->tm_mon + 1, t->tm_mday, WEEKDAY_CN[t->tm_wday % 7]);
    int date_w = st7305_utf8_width(1, date_buf);
    int date_x = (400 - date_w) / 2;
    st7305_draw_utf8(date_x, 140, 1, date_buf, true);

    if (!snap->time_valid) {
        /* 未校时徽章跟在日期行尾，不另占一行 */
        const char *bad = "未校时";
        int bw = st7305_utf8_width(1, bad);
        int bx = date_x + date_w + 12;
        st7305_fill_rect(bx, 139, bw + 12, 18, true);
        st7305_draw_utf8(bx + 6, 140, 1, bad, false);
    }
}

/* 图标承担指标语义，组内实测宽度后以group_cx为心水平居中。 */
static void draw_metric(int group_cx, bool is_temp, bool ok, const char *value)
{
    const char *v = ok ? value : "--";
    int vw = 32 * (int)strlen(v);
    int total = 12 + 10 + vw + (ok ? 6 + (is_temp ? 27 : 22) : 0);
    int x = group_cx - total / 2;

    if (is_temp) {
        icon_thermometer(x, 213);
    } else {
        icon_droplet(x, 213);
    }
    st7305_draw_text(x + 22, 206, 4, v, true);

    if (!ok) {
        return;
    }

    int value_end = x + 22 + vw;
    if (is_temp) {
        ui_v2_draw_temp_unit(value_end + 6, 206);
    } else {
        st7305_draw_text(value_end + 6, 222, 2, "%", true);
    }
}

static void draw(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);

    st7305_clear(false);
    ui_v2_draw_header("", &snap);

    struct tm t;
    time_t now = snap.now;
    localtime_r(&now, &t);
    draw_time_section(&t, &snap);

    ui_v2_draw_card(20, 188, 360, 76);

    char temp_buf[16], hum_buf[16];
    snprintf(temp_buf, sizeof(temp_buf), "%.1f", snap.temperature);
    snprintf(hum_buf, sizeof(hum_buf), "%.0f", snap.humidity);
    draw_metric(114, true, snap.env_ok, temp_buf);
    draw_metric(290, false, snap.env_ok, hum_buf);

    esp_err_t err = st7305_flush();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "flush失败: %s", esp_err_to_name(err));
    }
}

static void on_enter(void)
{
    draw();
}

static void on_tick(void)
{
    draw(); /* 每秒刷新以跟上时钟秒数(规范第12节: 首页时间每秒更新) */
}

static const ui_page_t s_page = {
    .name = "home",
    .on_enter = on_enter,
    .on_exit = NULL,
    .on_tick = on_tick,
    .on_key = NULL,
};

const ui_page_t *page_home_get(void)
{
    return &s_page;
}
