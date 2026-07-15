#include <stdio.h>
#include <time.h>

#include "display_st7305.h"
#include "esp_log.h"
#include "sys_state.h"
#include "ui_icons.h"
#include "ui_pages.h"
#include "ui_v2.h"

/*
 * 首页(桌面时钟主屏)：时间是主要视觉焦点，环境信息降为底部两列开放式摘要。
 * 只读取 sys_state 快照，不直接访问传感器/电池/网络/SD(规范第6节)。
 *
 * 布局(400×300，UI V2共享页头/页脚见 ui_v2.h):
 *   页头        y=0..34    "桌面" + SD/WiFi/电池图标
 *   时钟区      y=48..112  HH:MM 超大字 + 秒
 *   日期/校时   y=128..148
 *   分隔线      y=168
 *   温湿度摘要  y=184..228 两列开放式指标
 *   页脚        y=266..    KEY/VOICE/BOOT提示
 */

static const char *TAG = "page_home";

static const char *WEEKDAY_CN[] = { "星期日", "星期一", "星期二", "星期三",
                                    "星期四", "星期五", "星期六" };

static void draw_time_section(const struct tm *t, const sys_state_snapshot_t *snap)
{
    char buf[16];

    /* HH:MM 大字(8px × scale8 = 64px高) */
    snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
    int w = 5 * 8 * 8;
    int x = (400 - w) / 2;
    st7305_draw_text(x, 48, 8, buf, true);

    /* 秒: 小字贴在右下角基线 */
    snprintf(buf, sizeof(buf), "%02d", t->tm_sec);
    st7305_draw_text(x + w + 6, 96, 2, buf, true);

    char date_buf[48];
    snprintf(date_buf, sizeof(date_buf), "%d年%d月%d日 %s",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, WEEKDAY_CN[t->tm_wday % 7]);
    st7305_draw_utf8((400 - st7305_utf8_width(1, date_buf)) / 2, 128, 1, date_buf, true);

    if (snap->time_valid) {
        const char *ok = "时间已校准";
        st7305_draw_utf8((400 - st7305_utf8_width(1, ok)) / 2, 148, 1, ok, true);
    } else {
        /* 校时异常用反白短徽标，不用长警告句(规范: r0-ui-v2-spec.md 第7节) */
        const char *bad = "未校时";
        int bw = st7305_utf8_width(1, bad);
        int bx = (400 - (bw + 16)) / 2;
        st7305_fill_rect(bx, 144, bw + 16, 18, true);
        st7305_draw_utf8(bx + 8, 148, 1, bad, false);
    }
}

/* 两列开放式指标: 标签 + 大数值 + 单位，中间留白分隔，不用圆角卡片包裹。
 * 传感器读取失败时数值显示"--"，列位置保持不变(规范第7节) */
static void draw_column(int x, const char *label, bool ok, const char *value, const char *unit)
{
    st7305_draw_utf8(x, 184, 1, label, true);

    const char *v = ok ? value : "--";
    st7305_draw_text(x, 202, 4, v, true);
    int vw = st7305_utf8_width(4, v);
    st7305_draw_utf8(x + vw + 8, 218, 1, unit, true);
}

static void draw(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);

    st7305_clear(false);
    ui_v2_draw_header("桌面", &snap);

    struct tm t;
    time_t now = snap.now;
    localtime_r(&now, &t);
    draw_time_section(&t, &snap);

    ui_v2_hline(168);

    char temp_buf[16], hum_buf[16];
    snprintf(temp_buf, sizeof(temp_buf), "%.1f", snap.temperature);
    snprintf(hum_buf, sizeof(hum_buf), "%.0f", snap.humidity);
    draw_column(20, "温度", snap.env_ok, temp_buf, "C");
    draw_column(210, "湿度", snap.env_ok, hum_buf, "%");

    ui_v2_draw_footer("刷新");

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
    if (evt == UI_KEY_ACTION) {
        draw();
        return true;
    }
    return false;
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
