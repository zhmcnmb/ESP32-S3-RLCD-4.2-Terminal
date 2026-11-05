#include <stdio.h>
#include <string.h>

#include "display_st7305.h"
#include "esp_log.h"
#include "sys_state.h"
#include "ui_pages.h"
#include "ui_v2.h"

/*
 * 系统状态页：排障用。运行时长、复位原因、电池、信号、存储卡、内存水位。
 * 只读 sys_state 快照（资源水位由 sys_state_get_snapshot 现读填充）。
 *
 * 排版基准：
 *   中文 scale1 = 16px 高，ASCII scale1 = 8px 高，ASCII scale2 = 16px 高。
 *   所以"数字用 scale2、中文用 utf8 scale1"两者等高，可以放在同一行；
 *   每段文字之间至少留 6px，避免中文笔画和数字粘在一起。
 *   页头      8..24
 *   运行时长  38 标签 / 58 数值(16px) / 80 注脚
 *   分隔线    102
 *   设备三列  110 标签 / 130 数值 / 152 注脚
 *   分隔线    174
 *   内存两列  182 标签 / 202 数值 / 224 注脚
 *   分隔线    246
 *   固件版本  252 标签 / 272 版本号(8px)
 */

static const char *TAG = "page_status";

static const char *reset_reason_text(int reason)
{
    switch (reason) {
    case 1: return "上电";
    case 2: return "外部引脚";
    case 3: return "软件复位";
    case 4: return "系统崩溃";
    case 5: return "中断看门狗";
    case 6: return "任务看门狗";
    case 7: return "看门狗";
    case 8: return "深睡唤醒";
    case 9: return "电压跌落";
    case 10: return "SDIO";
    case 11: return "USB";
    case 12: return "JTAG";
    case 14: return "电源异常";
    case 15: return "CPU 锁死";
    default: return "未知";
    }
}

/* 字节 → 4 字符内的紧凑表示（212K / 7.9M / 512） */
static void format_bytes(uint32_t bytes, char *out, size_t out_len)
{
    if (bytes >= 1024U * 1024U) {
        snprintf(out, out_len, "%.1fM", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024U) {
        snprintf(out, out_len, "%luK", (unsigned long)(bytes / 1024U));
    } else {
        snprintf(out, out_len, "%lu", (unsigned long)bytes);
    }
}

/* 数值列：ASCII scale2（16px 高），与中文 utf8 scale1 等高 */
static void draw_cell(int x, int y, const char *label, const char *value, const char *caption)
{
    st7305_draw_utf8(x, y, 1, label, true);
    st7305_draw_text(x, y + 20, 2, value, true);
    st7305_draw_utf8(x, y + 42, 1, caption, true);
}

/* 中文数值列：整行都用 utf8，避免 ASCII 字库画不出汉字 */
static void draw_cell_cn(int x, int y, const char *label, const char *value, const char *caption)
{
    st7305_draw_utf8(x, y, 1, label, true);
    st7305_draw_utf8(x, y + 20, 1, value, true);
    st7305_draw_utf8(x, y + 42, 1, caption, true);
}

static void draw_battery_cell(const sys_state_snapshot_t *snap)
{
    char value[12], caption[32];
    if (snap->battery_ok) {
        snprintf(value, sizeof(value), "%d%%", snap->battery_percent);
        snprintf(caption, sizeof(caption), "%.2fV %s", (double)snap->battery_voltage,
                 snap->battery_charging ? "充电" : "放电");
    } else {
        snprintf(value, sizeof(value), "--");
        snprintf(caption, sizeof(caption), "未检测到");
    }
    draw_cell(28, 110, "电池", value, caption);
}

static void draw_wifi_cell(const sys_state_snapshot_t *snap)
{
    char value[12];
    if (!snap->wifi_connected) {
        draw_cell_cn(160, 110, "信号", "--", "未连接");
        return;
    }
    snprintf(value, sizeof(value), "%d", (int)snap->wifi_rssi);
    draw_cell(160, 110, "信号", value, "信号强度");
}

static void draw_memory_cells(const sys_state_snapshot_t *snap)
{
    char value[12], low[12], caption[24];
    format_bytes(snap->heap_free, value, sizeof(value));
    format_bytes(snap->heap_min, low, sizeof(low));
    snprintf(caption, sizeof(caption), "最低 %s", low);
    draw_cell(28, 182, "内部内存", value, caption);

    format_bytes(snap->psram_free, value, sizeof(value));
    format_bytes(snap->psram_min, low, sizeof(low));
    snprintf(caption, sizeof(caption), "最低 %s", low);
    draw_cell(216, 182, "外部内存", value, caption);
}

/* 运行时长：数字走 scale2，天数与复位原因合并到一行中文注脚 */
static void draw_uptime(const sys_state_snapshot_t *snap)
{
    uint32_t days = snap->uptime_s / 86400U;
    uint32_t hours = (snap->uptime_s % 86400U) / 3600U;
    uint32_t minutes = (snap->uptime_s % 3600U) / 60U;
    char clock[12], caption[48];
    snprintf(clock, sizeof(clock), "%02lu:%02lu", (unsigned long)hours, (unsigned long)minutes);
    if (days > 0) {
        snprintf(caption, sizeof(caption), "第 %lu 天 · %s", (unsigned long)days,
                 reset_reason_text(snap->reset_reason));
    } else {
        snprintf(caption, sizeof(caption), "今天 · %s", reset_reason_text(snap->reset_reason));
    }
    st7305_draw_utf8(28, 38, 1, "运行时长", true);
    st7305_draw_text(28, 58, 2, clock, true);
    st7305_draw_utf8(28, 80, 1, caption, true);
}

static void draw(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);

    st7305_clear(false);
    ui_v2_draw_header("状态", &snap);

    draw_uptime(&snap);
    st7305_draw_line(20, 102, 380, 102, true);

    draw_battery_cell(&snap);
    draw_wifi_cell(&snap);
    if (snap.sd_mounted) {
        draw_cell_cn(292, 110, "存储卡", "正常", "已挂载");
    } else {
        draw_cell_cn(292, 110, "存储卡", "--", "未插卡");
    }
    st7305_draw_line(20, 174, 380, 174, true);

    draw_memory_cells(&snap);
    st7305_draw_line(20, 246, 380, 246, true);

    st7305_draw_utf8(28, 252, 1, "固件版本", true);
    st7305_draw_text(28, 274, 1, snap.fw_version ? snap.fw_version : "-", true);

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
    draw();
}

static const ui_page_t s_page = {
    .name = "status",
    .on_enter = on_enter,
    .on_exit = NULL,
    .on_tick = on_tick,
    .on_key = NULL,
};

const ui_page_t *page_status_get(void)
{
    return &s_page;
}
