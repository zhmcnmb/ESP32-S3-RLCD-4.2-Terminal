#include <stdio.h>
#include <string.h>
#include <time.h>

#include "display_st7305.h"
#include "esp_log.h"
#include "ui_pages.h"
#include "weather_client.h"

/*
 * 天气页: 展示和风天气(QWeather)当前实况。数据经 weather_client 后台轮询
 * 官方HTTPS接口获取(15分钟一次)，本页不直接发HTTP请求(规范第5节)。
 *
 * 城市固定为常州·武进(对应 secrets.h 里的 QWEATHER_LOCATION)，纯展示用途，
 * 换城市需同步改这里的 CITY_NAME 和 secrets.h 的 LocationID。
 *
 * 布局(400×300):
 *   外框 2px
 *   标题栏      y=6..40    城市名 + 数据新鲜度/降级提示
 *   天气内容    y=64..248  大字温度 + 现象文字 + 湿度/风力
 *   底部提示    y=264
 */

static const char *TAG = "page_weather";
static const char *CITY_NAME = "常州武进";

#define REFRESH_TICKS 10 /* 客户端后台15分钟轮询一次，页面每10s重绘跟上新鲜度提示 */

static int s_tick = 0;

static void draw(void)
{
    st7305_clear(false);
    st7305_draw_rect(2, 2, 396, 296, 1, true);

    weather_now_t w;
    bool has_data = weather_client_get(&w);

    st7305_draw_utf8(20, 6, 2, CITY_NAME, true);

    char sub[32];
    if (!has_data) {
        snprintf(sub, sizeof(sub), "等待首次获取...");
    } else if (!w.available) {
        snprintf(sub, sizeof(sub), "天气接口不可达");
    } else {
        long age = (long)(time(NULL) - w.fetched_at);
        snprintf(sub, sizeof(sub), "数据于%ld分钟前", (age < 0 ? 0 : age) / 60);
    }
    st7305_draw_utf8(20, 40, 1, sub, true);
    st7305_fill_rect(20, 58, 360, 1, true);

    if (has_data && w.available) {
        char temp_buf[8];
        snprintf(temp_buf, sizeof(temp_buf), "%.0f", w.temperature);
        st7305_draw_text(40, 90, 6, temp_buf, true);
        int tw = 8 * 6 * (int)strlen(temp_buf);
        st7305_draw_utf8(40 + tw + 8, 96, 2, "C", true);

        st7305_draw_utf8(230, 100, 2, w.condition, true);

        char line[64];
        snprintf(line, sizeof(line), "湿度%d%%  %s", w.humidity, w.wind);
        st7305_draw_utf8(40, 190, 1, line, true);
    } else {
        st7305_draw_round_rect(20, 64, 360, 184, 8, true);
        const char *msg = has_data ? "无法连接天气服务，稍后自动重试" : "首次数据获取中，请稍候";
        st7305_draw_utf8(20 + (360 - st7305_utf8_width(1, msg)) / 2, 152, 1, msg, true);
    }

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
    if (++s_tick >= REFRESH_TICKS) {
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
