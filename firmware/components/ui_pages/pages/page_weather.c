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
 * 天气页：温度与天气图形是唯一主视觉；湿度和风降为底部摘要。
 * 页面只读取 sys_state 快照，不直接轮询天气服务(规范第6节)。
 * 城市固定为常州·武进，对应 secrets.h 的 QWEATHER_LOCATION。
 */

static const char *TAG = "page_weather";
static const char *CITY_NAME = "常州武进";

#define REFRESH_TICKS 10 /* 客户端后台15分钟轮询一次，页面每10s重绘跟上缓存变化 */

static int s_tick = 0;

static void draw_status_badge(const char *text)
{
    if (!text) {
        return;
    }
    int w = st7305_utf8_width(1, text) + 16;
    st7305_fill_rect(20, 40, w, 18, true);
    st7305_draw_utf8(28, 41, 1, text, false);
}

static void draw_weather_data(const sys_state_snapshot_t *snap)
{
    char temp_buf[8];
    snprintf(temp_buf, sizeof(temp_buf), "%.0f", snap->weather_temperature);
    st7305_draw_text(20, 62, 7, temp_buf, true);
    int temp_w = 8 * 7 * (int)strlen(temp_buf);
    st7305_draw_utf8(28 + temp_w, 96, 2, "C", true);

    icon_weather(292, 74, snap->weather_condition);
    int condition_w = st7305_utf8_width(1, snap->weather_condition);
    st7305_draw_utf8(312 - condition_w / 2, 126, 1, snap->weather_condition, true);

    st7305_fill_rect(20, 182, 48, 18, true);
    st7305_draw_utf8(28, 183, 1, "湿度", false);
    char humidity[8];
    snprintf(humidity, sizeof(humidity), "%d%%", snap->weather_humidity);
    st7305_draw_text(20, 208, 4, humidity, true);

    st7305_fill_rect(220, 182, 32, 18, true);
    st7305_draw_utf8(228, 183, 1, "风", false);
    st7305_draw_utf8(220, 214, 1, snap->weather_wind, true);
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
    draw_status_badge(badge);

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

static bool on_key(ui_key_event_t evt)
{
    (void)evt;
    return false;
}

static const ui_page_t s_page = {
    .name = "weather",
    .on_enter = on_enter,
    .on_exit = NULL,
    .on_tick = on_tick,
    .on_key = on_key,
};

const ui_page_t *page_weather_get(void)
{
    return &s_page;
}
