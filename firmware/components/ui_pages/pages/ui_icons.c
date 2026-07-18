#include "ui_icons.h"

#include <string.h>

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

void icon_thermometer(int x, int y)
{
    /* 杆: 细长圆角矩形，内部填充示意水银柱 */
    st7305_draw_round_rect(x + 4, y, 4, 10, 2, ICON_BLACK);
    st7305_fill_rect(x + 5, y + 2, 2, 7, ICON_BLACK);

    /* 底部球 */
    st7305_draw_round_rect(x + 2, y + 9, 8, 8, 4, ICON_BLACK);
    st7305_fill_rect(x + 4, y + 11, 4, 4, ICON_BLACK);
}

void icon_droplet(int x, int y)
{
    /* 尖顶两条斜边 + 圆润底部 */
    st7305_draw_line(x + 6, y, x + 1, y + 8, ICON_BLACK);
    st7305_draw_line(x + 6, y, x + 11, y + 8, ICON_BLACK);
    st7305_draw_round_rect(x, y + 6, 12, 10, 5, ICON_BLACK);
}

void icon_wind(int x, int y)
{
    st7305_draw_line(x, y + 3, x + 13, y + 3, ICON_BLACK);
    st7305_draw_line(x + 13, y + 3, x + 16, y + 1, ICON_BLACK);
    st7305_draw_line(x, y + 8, x + 17, y + 8, ICON_BLACK);
    st7305_draw_line(x, y + 13, x + 11, y + 13, ICON_BLACK);
    st7305_draw_line(x + 11, y + 13, x + 14, y + 11, ICON_BLACK);
}

/* 云朵轮廓: 底部长圆角矩形 + 顶部两个凸起，近似云的锯齿感 */
static void draw_cloud_body(int x, int y)
{
    st7305_draw_round_rect(x + 2, y + 16, 36, 16, 8, ICON_BLACK);
    st7305_draw_round_rect(x + 8, y + 6, 16, 16, 8, ICON_BLACK);
    st7305_draw_round_rect(x + 20, y + 10, 14, 14, 7, ICON_BLACK);
}

void icon_weather(int x, int y, const char *condition)
{
    if (!condition) {
        condition = "";
    }
    if (strstr(condition, "雨")) {
        draw_cloud_body(x, y);
        st7305_draw_line(x + 10, y + 34, x + 6, y + 40, ICON_BLACK);
        st7305_draw_line(x + 20, y + 34, x + 16, y + 40, ICON_BLACK);
        st7305_draw_line(x + 30, y + 34, x + 26, y + 40, ICON_BLACK);
    } else if (strstr(condition, "雪")) {
        draw_cloud_body(x, y);
        st7305_fill_rect(x + 8, y + 36, 2, 2, ICON_BLACK);
        st7305_fill_rect(x + 18, y + 38, 2, 2, ICON_BLACK);
        st7305_fill_rect(x + 28, y + 36, 2, 2, ICON_BLACK);
    } else if (strstr(condition, "雾") || strstr(condition, "霾")) {
        for (int i = 0; i < 4; i++) {
            st7305_draw_line(x + 2, y + 10 + i * 8, x + 38, y + 10 + i * 8, ICON_BLACK);
        }
    } else if (strstr(condition, "云") || strstr(condition, "阴")) {
        draw_cloud_body(x, y);
    } else if (strstr(condition, "晴")) {
        st7305_draw_round_rect(x + 12, y + 12, 16, 16, 8, ICON_BLACK);
        st7305_draw_line(x + 20, y, x + 20, y + 8, ICON_BLACK);
        st7305_draw_line(x + 20, y + 32, x + 20, y + 40, ICON_BLACK);
        st7305_draw_line(x, y + 20, x + 8, y + 20, ICON_BLACK);
        st7305_draw_line(x + 32, y + 20, x + 40, y + 20, ICON_BLACK);
        st7305_draw_line(x + 6, y + 6, x + 12, y + 12, ICON_BLACK);
        st7305_draw_line(x + 34, y + 6, x + 28, y + 12, ICON_BLACK);
        st7305_draw_line(x + 6, y + 34, x + 12, y + 28, ICON_BLACK);
        st7305_draw_line(x + 34, y + 34, x + 28, y + 28, ICON_BLACK);
    } else {
        /* 未收录类别: 通用圆形天气标记 */
        st7305_draw_round_rect(x + 8, y + 8, 24, 24, 12, ICON_BLACK);
    }
}
