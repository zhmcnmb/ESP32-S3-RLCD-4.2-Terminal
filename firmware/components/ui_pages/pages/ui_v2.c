#include "ui_v2.h"

#include "display_st7305.h"
#include "ui_icons.h"
#define MARGIN 20

/* 共享页头只保留身份与设备状态；内容区各页用圆角卡片分组组织。 */

void ui_v2_draw_header(const char *title, const sys_state_snapshot_t *snap)
{
    if (title && title[0]) {
        st7305_fill_rect(MARGIN, 8, 4, 16, true);
        st7305_draw_utf8(MARGIN + 12, 8, 1, title, true);
    }

    /* 状态图标保持固定位置；无论页面内容如何变化，设备自身状态都无需抢占主视觉。 */
    icon_sd(316, 7, snap->sd_mounted);

    int wifi_level = snap->wifi_connected ? 3 : 0;
    icon_wifi(336, 7, wifi_level);

    icon_battery(360, 8, snap->battery_ok ? snap->battery_percent : 0,
                 snap->battery_ok && snap->battery_charging);
}

/* 等待/离线才使用居中态；正常页面不借此补充解释性文字。 */
void ui_v2_draw_empty_state(int y, const char *primary, const char *secondary)
{
    st7305_draw_utf8((400 - st7305_utf8_width(2, primary)) / 2, y, 2, primary, true);
    if (secondary) {
        st7305_draw_utf8((400 - st7305_utf8_width(1, secondary)) / 2, y + 40, 1, secondary, true);
    }
}

void ui_v2_draw_card(int x, int y, int w, int h)
{
    st7305_draw_round_rect(x, y, w, h, 12, true);
}

int ui_v2_draw_temp_unit(int x, int y)
{
    st7305_draw_round_rect(x, y, 8, 8, 4, true);
    st7305_draw_text(x + 11, y, 2, "C", true);
    return x + 27;
}

void ui_v2_draw_status_badge(const char *text)
{
    if (!text) {
        return;
    }

    int w = st7305_utf8_width(1, text) + 16;
    int x = 400 - MARGIN - w;
    st7305_fill_rect(x, 34, w, 18, true);
    st7305_draw_utf8(x + 8, 35, 1, text, false);
}
