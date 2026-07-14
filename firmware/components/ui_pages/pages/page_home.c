#include <stdio.h>
#include <time.h>

#include "battery.h"
#include "display_st7305.h"
#include "esp_log.h"
#include "net_manager.h"
#include "sensor_shtc3.h"
#include "storage_sd.h"
#include "time_service.h"
#include "ui_icons.h"
#include "ui_pages.h"

/*
 * 首页布局(400×300):
 *   外框        2px描边留白
 *   顶部状态栏  y=8..27    电池/WiFi/SD图标 + 分隔线
 *   时钟区      y=38..112  HH:MM 超大字 + 秒
 *   日期行      y=124..140 中文年月日星期 + 分隔线
 *   数据卡片    y=154..258 温度 | 湿度 双卡片(图标+标题+大数值+单位)
 *   底部        y=266..    分隔线 + 提示
 */

static const char *TAG = "page_home";

static shtc3_reading_t s_env;
static bool s_env_ok = false;
static battery_status_t s_bat;
static bool s_bat_ok = false;
static int s_tick = 0;

/* 传感器与电池读取都有开销，不必每秒刷 */
#define SENSOR_REFRESH_TICKS 10

static const char *WEEKDAY_CN[] = { "星期日", "星期一", "星期二", "星期三",
                                    "星期四", "星期五", "星期六" };

static void draw_status_bar(void)
{
    icon_battery(20, 8, s_bat_ok ? s_bat.percent : 0, s_bat_ok && s_bat.charging);

    if (s_bat_ok) {
        char buf[16];
        if (s_bat.charging) {
            snprintf(buf, sizeof(buf), "%.2fV", s_bat.voltage);
        } else {
            snprintf(buf, sizeof(buf), "%d%%", s_bat.percent);
        }
        st7305_draw_text(52, 11, 1, buf, true);
    }

    /* 右侧: SD + WiFi */
    icon_sd(340, 7, storage_sd_is_mounted());
    int wifi_level = (net_get_state() == NET_CONNECTED) ? 3
                   : (net_get_state() == NET_CONNECTING) ? 1 : 0;
    icon_wifi(364, 7, wifi_level);

    st7305_fill_rect(20, 28, 360, 1, true);
}

static void draw_clock(const struct tm *t)
{
    char buf[16];

    /* HH:MM 大字(8px × scale8 = 64px高) */
    snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
    int w = 5 * 8 * 8;
    int x = (400 - w) / 2;
    st7305_draw_text(x, 38, 8, buf, true);

    /* 秒: 小字贴在右下角基线 */
    snprintf(buf, sizeof(buf), "%02d", t->tm_sec);
    st7305_draw_text(x + w + 6, 84, 2, buf, true);

    if (!time_service_is_valid()) {
        const char *warn = "时间未校准";
        st7305_draw_utf8((400 - st7305_utf8_width(1, warn)) / 2, 112, 1, warn, true);
    }
}

static void draw_date(const struct tm *t)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%d年%d月%d日 %s",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, WEEKDAY_CN[t->tm_wday % 7]);
    st7305_draw_utf8((400 - st7305_utf8_width(1, buf)) / 2, 124, 1, buf, true);

    st7305_fill_rect(20, 144, 360, 1, true);
}

/* 数据卡片: 圆角框 + 图标 + 标题 + 大数值 + 单位。
 * icon 由调用方传入(温度计/水滴)，用图形区分两张卡片，比纯文字更易一眼辨识。 */
static void draw_card(int x, int y, int w, int h, void (*icon)(int, int),
                      const char *title, const char *value, const char *unit)
{
    st7305_draw_round_rect(x, y, w, h, 8, true);

    icon(x + 14, y + 14);
    st7305_draw_utf8(x + 34, y + 14, 1, title, true);

    int vw = st7305_utf8_width(4, value); /* value是ASCII数字，用等宽公式即可 */
    st7305_draw_text(x + 14, y + 42, 4, value, true);
    st7305_draw_utf8(x + 14 + vw + 8, y + 58, 1, unit, true);

    (void)h;
}

static void draw_data_cards(void)
{
    const int y = 154, h = 104, w = 172;

    if (!s_env_ok) {
        st7305_draw_round_rect(20, y, 360, h, 8, true);
        const char *msg = "传感器读取失败";
        st7305_draw_utf8((400 - st7305_utf8_width(1, msg)) / 2, y + 44, 1, msg, true);
        return;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", s_env.temperature);
    draw_card(20, y, w, h, icon_thermometer, "温度", buf, "C");

    snprintf(buf, sizeof(buf), "%.0f", s_env.humidity);
    draw_card(208, y, w, h, icon_droplet, "湿度", buf, "%");
}

static void draw(void)
{
    st7305_clear(false);
    st7305_draw_rect(2, 2, 396, 296, 1, true); /* 外框，避免元素像悬浮在白底上 */

    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    draw_status_bar();
    draw_clock(&t);
    draw_date(&t);
    draw_data_cards();

    st7305_fill_rect(20, 266, 360, 1, true);
    const char *hint = "KEY 切换页面";
    st7305_draw_utf8((400 - st7305_utf8_width(1, hint)) / 2, 276, 1, hint, true);

    esp_err_t err = st7305_flush();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "flush失败: %s", esp_err_to_name(err));
    }
}

static void refresh_sensors(void)
{
    s_env_ok = (shtc3_read(&s_env) == ESP_OK);
    s_bat_ok = (battery_read(&s_bat) == ESP_OK);
}

static void on_enter(void)
{
    refresh_sensors();
    s_tick = 0;
    draw();
}

static void on_tick(void)
{
    if (++s_tick >= SENSOR_REFRESH_TICKS) {
        s_tick = 0;
        refresh_sensors();
    }
    draw();
}

static bool on_key(ui_key_event_t evt)
{
    if (evt == UI_KEY_ACTION) {
        refresh_sensors();
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
