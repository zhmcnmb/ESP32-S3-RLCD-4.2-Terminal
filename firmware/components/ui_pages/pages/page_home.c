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
 *   页头        y=0..34    状态图标(无重复标题)
 *   时钟区      y=56..120  HH:MM 超大字 + 秒
 *   日期/校时   y=136..174 仅异常时显示"未校时"
 *   温湿度摘要  y=216..248 两列开放式指标
 */

static const char *TAG = "page_home";

static const char *WEEKDAY_CN[] = { "星期日", "星期一", "星期二", "星期三",
                                    "星期四", "星期五", "星期六" };

static void draw_time_section(const struct tm *t, const sys_state_snapshot_t *snap)
{
    char buf[16];

    /* 时钟独占上半屏：大数字本身承担主视觉，不再放进卡片或取景框。 */
    snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
    st7305_draw_text(40, 56, 8, buf, true);

    snprintf(buf, sizeof(buf), "%02d", t->tm_sec);
    st7305_draw_text(366, 104, 2, buf, true);

    char date_buf[48];
    snprintf(date_buf, sizeof(date_buf), "%d月%d日 %s",
             t->tm_mon + 1, t->tm_mday, WEEKDAY_CN[t->tm_wday % 7]);
    st7305_draw_utf8((400 - st7305_utf8_width(1, date_buf)) / 2, 136, 1, date_buf, true);

    if (!snap->time_valid) {
        const char *bad = "未校时";
        int bw = st7305_utf8_width(1, bad);
        int bx = (400 - (bw + 16)) / 2;
        st7305_fill_rect(bx, 156, bw + 16, 18, true);
        st7305_draw_utf8(bx + 8, 157, 1, bad, false);
    }
}

/* 图标承担指标语义，开放数值区保留给环境读数。 */
static void draw_metric(int x, bool is_temp, bool ok, const char *value)
{
    const char *v = ok ? value : "--";
    int value_x = x + (is_temp ? 22 : 20);

    if (is_temp) {
        icon_thermometer(x, 220);
    } else {
        icon_droplet(x, 220);
    }
    st7305_draw_text(value_x, 216, 4, v, true);

    if (!ok) {
        return;
    }

    int value_end = value_x + 32 * (int)strlen(v);
    if (is_temp) {
        ui_v2_draw_temp_unit(value_end + 8, 216);
    } else {
        st7305_draw_text(value_end + 6, 232, 2, "%", true);
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

    ui_v2_draw_dots_divider(196);

    char temp_buf[16], hum_buf[16];
    snprintf(temp_buf, sizeof(temp_buf), "%.1f", snap.temperature);
    snprintf(hum_buf, sizeof(hum_buf), "%.0f", snap.humidity);
    draw_metric(60, true, snap.env_ok, temp_buf);
    draw_metric(268, false, snap.env_ok, hum_buf);

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

static bool on_key(ui_key_event_t evt)
{
    (void)evt;
    return false; /* 普通只读页不消费 BOOT；KEY NEXT 由 ui_manager 处理 */
}

static const ui_page_t s_page = {
    .name = "home",
    .on_enter = on_enter,
    .on_exit = NULL,
    .on_tick = on_tick,
    .on_key = on_key,
};

const ui_page_t *page_home_get(void)
{
    return &s_page;
}
