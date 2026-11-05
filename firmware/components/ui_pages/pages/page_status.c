#include <stdio.h>
#include <string.h>

#include "display_st7305.h"
#include "esp_log.h"
#include "sys_state.h"
#include "ui_pages.h"
#include "ui_v2.h"

/*
 * 系统状态页：排障用。运行时长、复位原因、电池、WiFi 信号、SD、堆与 PSRAM 水位。
 * 只读 sys_state 快照（资源水位由 sys_state_get_snapshot 现读填充），
 * 不直接调用 esp_* 接口，符合页面分层约束。
 *
 * 布局(400×300)，与新语言一致：无卡片，靠字号层级 + 三条分隔线分组。
 *   页头      y=0..34
 *   运行时长  y=46..92   UPTIME + 大号时长 + 复位原因
 *   设备状态  y=112..156 BATTERY / WIFI / SD 三列
 *   内存水位  y=178..222 HEAP / PSRAM 两列（当前 / 历史最低）
 *   固件信息  y=244..270
 */

static const char *TAG = "page_status";

static const char *reset_reason_text(int reason)
{
    switch (reason) {
    case 1: return "POWERON";
    case 2: return "EXT PIN";
    case 3: return "SOFTWARE";
    case 4: return "PANIC";
    case 5: return "INT WDT";
    case 6: return "TASK WDT";
    case 7: return "WDT";
    case 8: return "DEEP SLEEP";
    case 9: return "BROWNOUT";
    case 10: return "SDIO";
    case 11: return "USB";
    case 12: return "JTAG";
    case 14: return "PWR GLITCH";
    case 15: return "CPU LOCKUP";
    default: return "UNKNOWN";
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

static void format_uptime(uint32_t seconds, char *out, size_t out_len)
{
    uint32_t days = seconds / 86400U;
    uint32_t hours = (seconds % 86400U) / 3600U;
    uint32_t minutes = (seconds % 3600U) / 60U;
    if (days > 0) {
        snprintf(out, out_len, "%lud %02lu:%02lu", (unsigned long)days,
                 (unsigned long)hours, (unsigned long)minutes);
    } else {
        snprintf(out, out_len, "%02lu:%02lu", (unsigned long)hours, (unsigned long)minutes);
    }
}

/* 一列：小标签 + 大字数值 + 注脚 */
static void draw_cell(int x, int y, const char *label, const char *value, const char *caption)
{
    st7305_draw_text(x, y, 1, label, true);
    st7305_draw_text(x, y + 14, 2, value, true);
    st7305_draw_text(x, y + 36, 1, caption, true);
}

static void draw_battery_cell(const sys_state_snapshot_t *snap)
{
    char value[12], caption[16];
    if (snap->battery_ok) {
        snprintf(value, sizeof(value), "%d%%", snap->battery_percent);
        snprintf(caption, sizeof(caption), "%.2fV %s", (double)snap->battery_voltage,
                 snap->battery_charging ? "CHG" : "BAT");
    } else {
        snprintf(value, sizeof(value), "--");
        snprintf(caption, sizeof(caption), "NO GAUGE");
    }
    draw_cell(32, 112, "BATTERY", value, caption);
}

static void draw_wifi_cell(const sys_state_snapshot_t *snap)
{
    char value[12];
    if (!snap->wifi_connected) {
        draw_cell(164, 112, "WIFI", "--", "OFFLINE");
        return;
    }
    snprintf(value, sizeof(value), "%d", (int)snap->wifi_rssi);
    draw_cell(164, 112, "WIFI", value, "dBm");
}

static void draw_memory_cells(const sys_state_snapshot_t *snap)
{
    char value[12], low[12], caption[20];
    format_bytes(snap->heap_free, value, sizeof(value));
    format_bytes(snap->heap_min, low, sizeof(low));
    snprintf(caption, sizeof(caption), "MIN %s", low);
    draw_cell(32, 178, "HEAP", value, caption);

    format_bytes(snap->psram_free, value, sizeof(value));
    format_bytes(snap->psram_min, low, sizeof(low));
    snprintf(caption, sizeof(caption), "MIN %s", low);
    draw_cell(216, 178, "PSRAM", value, caption);
}

static void draw(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);

    st7305_clear(false);
    ui_v2_draw_header("状态", &snap);

    char uptime[16], caption[32];
    format_uptime(snap.uptime_s, uptime, sizeof(uptime));
    snprintf(caption, sizeof(caption), "RESET %s", reset_reason_text(snap.reset_reason));
    st7305_draw_text(32, 46, 1, "UPTIME", true);
    st7305_draw_text(32, 58, 2, uptime, true);
    st7305_draw_text(32, 82, 1, caption, true);
    st7305_draw_line(20, 100, 380, 100, true);

    draw_battery_cell(&snap);
    draw_wifi_cell(&snap);
    draw_cell(296, 112, "SD", snap.sd_mounted ? "OK" : "--",
              snap.sd_mounted ? "MOUNTED" : "NO CARD");
    st7305_draw_line(20, 166, 380, 166, true);

    draw_memory_cells(&snap);
    st7305_draw_line(20, 232, 380, 232, true);

    st7305_draw_text(32, 244, 1, "FIRMWARE", true);
    st7305_draw_text(32, 258, 1, snap.fw_version ? snap.fw_version : "-", true);

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
    draw(); /* 运行时长按分钟级别变化，保持 1s 刷新以兼顾响应 */
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
