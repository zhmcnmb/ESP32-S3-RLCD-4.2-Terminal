#include <stdio.h>
#include <string.h>

#include "display_st7305.h"
#include "esp_log.h"
#include "sys_state.h"
#include "ui_icons.h"
#include "ui_pages.h"
#include "ui_v2.h"

/*
 * 天气页：温度与天气图形是唯一主视觉；湿度和风降为底部摘要。
 * 页面只读取 sys_state 快照，不直接轮询天气服务(规范第6节)。
 * 城市固定为常州·武进，对应 secrets.h 的 QWEATHER_LOCATION。
 */

static const char *TAG = "page_weather";
static const char *CITY_NAME = "常州武进";

#define REFRESH_TICKS 10 /* 客户端后台15分钟轮询一次，页面每10s重绘跟上缓存变化 */

static int s_tick = 0;


static void draw_weather_data(const sys_state_snapshot_t *snap)
{
    char temp_buf[8];
    snprintf(temp_buf, sizeof(temp_buf), "%.0f", snap->weather_temperature);

    ui_v2_draw_card(20, 44, 360, 124);
    st7305_draw_text(44, 64, 8, temp_buf, true);
    int temp_w = 64 * (int)strlen(temp_buf);
    ui_v2_draw_temp_unit(44 + temp_w + 10, 68);

    icon_weather(292, 62, snap->weather_condition);
    int w2 = st7305_utf8_width(2, snap->weather_condition);
    if (w2 <= 160) {
        st7305_draw_utf8(312 - w2 / 2, 116, 2, snap->weather_condition, true);
    } else {
        int w1 = st7305_utf8_width(1, snap->weather_condition);
        int x = 312 - w1 / 2;
        st7305_draw_utf8(x > 216 ? x : 216, 122, 1, snap->weather_condition, true);
    }

    ui_v2_draw_card(20, 180, 360, 72);

    /* 两组摘要各占卡片一半，组内实测宽度后居中 */
    char humidity[8];
    snprintf(humidity, sizeof(humidity), "%d%%", snap->weather_humidity);
    int hum_w = 32 * (int)strlen(humidity);
    int hx = 110 - (12 + 10 + hum_w) / 2;
    icon_droplet(hx, 210);
    st7305_draw_text(hx + 22, 202, 4, humidity, true);

    int wind_w = st7305_utf8_width(1, snap->weather_wind);
    int wx = 290 - (18 + 8 + wind_w) / 2;
    icon_wind(wx, 210);
    st7305_draw_utf8(wx + 26, 210, 1, snap->weather_wind, true);
}

static void draw(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);

    st7305_clear(false);
    ui_v2_draw_header(CITY_NAME, &snap);

    bool has_data = snap.weather_updated_at != 0;
    bool stale = has_data && !snap.weather_available;
    const char *badge = stale ? "旧" : (!has_data && !snap.wifi_connected ? "离线" : NULL);
    ui_v2_draw_status_badge(badge);

    if (has_data) {
        draw_weather_data(&snap);
    } else {
        ui_v2_draw_empty_state(132, snap.wifi_connected ? "正在获取" : "离线", NULL);
    }

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

static const ui_page_t s_page = {
    .name = "weather",
    .on_enter = on_enter,
    .on_exit = NULL,
    .on_tick = on_tick,
    .on_key = NULL,
};

const ui_page_t *page_weather_get(void)
{
    return &s_page;
}
