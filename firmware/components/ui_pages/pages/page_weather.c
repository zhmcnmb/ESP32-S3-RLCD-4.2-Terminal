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
 * 天气页(天气海报)：温度和天气图形构成主要识别。只读取 sys_state 快照，
 * 不直接轮询天气服务(规范第6节)。
 *
 * 城市固定为常州·武进(对应 secrets.h 里桥接服务侧的 QWeather LocationID)，
 * 纯展示用途；weather_client 通过 bridge/server.py 代理获取天气数据，
 * 详见 docs/CLAUDE.md "P3 天气代理"一节。
 *
 * 布局(400×300，UI V2共享页头/页脚见 ui_v2.h):
 *   页头        y=0..34    城市名 + SD/WiFi/电池图标
 *   新鲜度/旧数据徽标  y=44
 *   温度+图标   y=90..160  大字温度(左) + 天气图标/现象(右)
 *   三列摘要    y=210      湿度 | 风 | 数据状态
 *   页脚        y=266..    KEY/VOICE/BOOT提示
 */

static const char *TAG = "page_weather";
static const char *CITY_NAME = "常州武进";

#define REFRESH_TICKS 10 /* 客户端后台15分钟轮询一次，页面每10s重绘跟上新鲜度提示 */

static int s_tick = 0;

static void draw(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);

    st7305_clear(false);
    ui_v2_draw_header(CITY_NAME, &snap);

    bool has_data = snap.weather_updated_at != 0;
    bool stale = has_data && !snap.weather_available;

    char sub[32];
    if (!has_data) {
        snprintf(sub, sizeof(sub), "首次数据获取中");
    } else if (stale) {
        snprintf(sub, sizeof(sub), "旧数据");
    } else {
        long age = (long)(snap.now - snap.weather_updated_at);
        snprintf(sub, sizeof(sub), "%ld分钟前", (age < 0 ? 0 : age) / 60);
    }
    st7305_draw_utf8(400 - 20 - st7305_utf8_width(1, sub), 44, 1, sub, true);

    if (has_data) {
        char temp_buf[8];
        snprintf(temp_buf, sizeof(temp_buf), "%.0f", snap.weather_temperature);
        st7305_draw_text(20, 90, 6, temp_buf, true);
        int tw = 8 * 6 * (int)strlen(temp_buf);
        st7305_draw_utf8(20 + tw + 8, 96, 2, "C", true);

        icon_weather(260, 80, snap.weather_condition);
        st7305_draw_utf8(260 + (40 - st7305_utf8_width(1, snap.weather_condition)) / 2, 126, 1,
                         snap.weather_condition, true);

        char humidity_col[16], wind_col[48], status_col[16];
        snprintf(humidity_col, sizeof(humidity_col), "湿度 %d%%", snap.weather_humidity);
        snprintf(wind_col, sizeof(wind_col), "风 %s", snap.weather_wind);
        snprintf(status_col, sizeof(status_col), "数据 %s", stale ? "旧" : "正常");

        st7305_draw_utf8(20, 210, 1, humidity_col, true);
        st7305_draw_utf8(160, 210, 1, wind_col, true);
        st7305_draw_utf8(320, 210, 1, status_col, true);
    } else {
        ui_v2_draw_empty_state(120, "正在获取", "首次连接桥接服务中");
    }

    ui_v2_draw_footer("刷新");

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
