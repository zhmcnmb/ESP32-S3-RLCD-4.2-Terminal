#include "ui_v2.h"

#include <stdio.h>

#include "display_st7305.h"
#include "ui_icons.h"

/* 页头/页脚公共左右安全边距 */
#define MARGIN 20
#define CONTENT_W (400 - 2 * MARGIN)

void ui_v2_draw_header(const char *title, const sys_state_snapshot_t *snap)
{
    st7305_draw_utf8(MARGIN, 6, 2, title, true);

    /* 图标固定顺序(从左到右: SD/WiFi/电池)、固定x坐标——数据不可用只改内部
     * 形态(叉/空心/无电量条)，不挪位置，保证四页状态栏视觉锚点一致 */
    icon_sd(316, 7, snap->sd_mounted);

    int wifi_level = snap->wifi_connected ? 3 : 0;
    icon_wifi(336, 7, wifi_level);

    icon_battery(360, 8, snap->battery_ok ? snap->battery_percent : 0,
                snap->battery_ok && snap->battery_charging);

    ui_v2_hline(34);
}

void ui_v2_draw_footer(const char *action_label)
{
    ui_v2_hline(266);

    char hint[64];
    snprintf(hint, sizeof(hint), "KEY 翻页   长按说话   BOOT %s", action_label);
    st7305_draw_utf8((400 - st7305_utf8_width(1, hint)) / 2, 278, 1, hint, true);
}

void ui_v2_hline(int y)
{
    st7305_fill_rect(MARGIN, y, CONTENT_W, 1, true);
}

void ui_v2_draw_empty_state(int y, const char *primary, const char *secondary)
{
    st7305_draw_utf8((400 - st7305_utf8_width(2, primary)) / 2, y, 2, primary, true);
    if (secondary) {
        st7305_draw_utf8((400 - st7305_utf8_width(1, secondary)) / 2, y + 40, 1, secondary, true);
    }
}
