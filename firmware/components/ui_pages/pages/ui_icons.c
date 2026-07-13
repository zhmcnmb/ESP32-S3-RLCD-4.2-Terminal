#include "ui_icons.h"

#include "display_st7305.h"

#define ICON_BLACK true

void icon_battery(int x, int y, int percent, bool charging)
{
    const int w = 24, h = 12;

    /* 外壳 + 正极触点 */
    st7305_draw_rect(x, y + 2, w, h, 1, ICON_BLACK);
    st7305_fill_rect(x + w, y + 5, 2, 6, ICON_BLACK);

    if (charging) {
        /* 充电中: 画闪电，不显示电量条(充电时电压不代表真实电量) */
        st7305_draw_line(x + 13, y + 4, x + 9, y + 9, ICON_BLACK);
        st7305_draw_line(x + 9, y + 9, x + 13, y + 9, ICON_BLACK);
        st7305_draw_line(x + 13, y + 9, x + 9, y + 13, ICON_BLACK);
        st7305_draw_line(x + 14, y + 4, x + 10, y + 9, ICON_BLACK);
        st7305_draw_line(x + 14, y + 9, x + 10, y + 13, ICON_BLACK);
        return;
    }

    /* 电量条: 内部留1px间隙 */
    int fill = (w - 4) * percent / 100;
    if (fill > 0) {
        st7305_fill_rect(x + 2, y + 4, fill, h - 4, ICON_BLACK);
    }
}

void icon_wifi(int x, int y, int level)
{
    if (level <= 0) {
        /* 断开: 画叉 */
        st7305_draw_line(x + 3, y + 4, x + 13, y + 13, ICON_BLACK);
        st7305_draw_line(x + 13, y + 4, x + 3, y + 13, ICON_BLACK);
        return;
    }

    /* 信号点 */
    st7305_fill_rect(x + 7, y + 12, 3, 3, ICON_BLACK);

    /* 由内到外三段弧，用折线近似(单色小图标不必真画圆弧) */
    if (level >= 1) {
        st7305_draw_line(x + 5, y + 9, x + 8, y + 7, ICON_BLACK);
        st7305_draw_line(x + 8, y + 7, x + 11, y + 9, ICON_BLACK);
    }
    if (level >= 2) {
        st7305_draw_line(x + 3, y + 7, x + 8, y + 4, ICON_BLACK);
        st7305_draw_line(x + 8, y + 4, x + 13, y + 7, ICON_BLACK);
    }
    if (level >= 3) {
        st7305_draw_line(x + 1, y + 5, x + 8, y + 1, ICON_BLACK);
        st7305_draw_line(x + 8, y + 1, x + 15, y + 5, ICON_BLACK);
    }
}

void icon_sd(int x, int y, bool present)
{
    const int w = 12, h = 15;

    /* 卡体: 左上角切角 */
    st7305_draw_line(x + 3, y + 1, x + w - 1, y + 1, ICON_BLACK);       /* 上 */
    st7305_draw_line(x, y + 4, x, y + h - 1, ICON_BLACK);               /* 左 */
    st7305_draw_line(x + w - 1, y + 1, x + w - 1, y + h - 1, ICON_BLACK); /* 右 */
    st7305_draw_line(x, y + h - 1, x + w - 1, y + h - 1, ICON_BLACK);   /* 下 */
    st7305_draw_line(x, y + 4, x + 3, y + 1, ICON_BLACK);               /* 切角 */

    if (present) {
        /* 已插卡: 内部触点条 */
        st7305_fill_rect(x + 3, y + 4, 2, 4, ICON_BLACK);
        st7305_fill_rect(x + 7, y + 4, 2, 4, ICON_BLACK);
    } else {
        /* 无卡: 斜杠 */
        st7305_draw_line(x + 1, y + h - 2, x + w - 2, y + 2, ICON_BLACK);
    }
}
