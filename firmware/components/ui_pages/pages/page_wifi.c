#include <stdio.h>
#include <string.h>

#include "display_st7305.h"
#include "esp_log.h"
#include "sys_state.h"
#include "ui_pages.h"
#include "ui_v2.h"
#include "wifi_setup.h"

/*
 * WiFi 设置页：OFF 时显示当前连接状态与SSID，BOOT短按开始扫描进入配网流程。
 * 配网激活后消费全部按键翻译成命令投递给 wifi_setup(模态流程，翻页被冻结，
 * 列表态BOOT长按逐层退出)。页面只读 sys_state 快照，不直接触碰WiFi。
 * 密码逐位输入：整个字符集按类别分行铺成网格，候选反白高亮，KEY短/长按
 * 沿一维候选序前后移动(确认后停在原地，连续字符零成本)，末尾是"删"/"成"。
 */

static const char *TAG = "page_wifi";

#define LIST_ROWS 6 /* AP列表每屏行数 */
#define PWD_SHOW_MAX 40 /* 密码明文窗口，超出显示末40位 */

static uint32_t s_last_version;

static void draw_off(const sys_state_snapshot_t *snap)
{
    ui_v2_draw_card(20, 60, 360, 120);
    st7305_draw_utf8(40, 84, 1, snap->wifi_connected ? "已连接" : "未连接", true);
    st7305_draw_utf8(40, 116, 2, snap->wifi_setup.ssid[0] ? snap->wifi_setup.ssid : "--", true);
    st7305_draw_utf8(40, 152, 1, "BOOT短按 扫描网络", true);
}

static void draw_ap_list(const sys_wifi_setup_view_t *view)
{
    st7305_draw_utf8(20, 46, 1, "选择网络", true);
    if (view->ap_count == 0) {
        ui_v2_draw_empty_state(140, "未扫描到网络", NULL);
        return;
    }
    int offset = 0;
    if (view->ap_sel >= LIST_ROWS) {
        offset = view->ap_sel - LIST_ROWS + 1;
    }
    for (int i = 0; i < LIST_ROWS && offset + i < view->ap_count; i++) {
        int idx = offset + i;
        int y = 72 + i * 34;
        bool selected = (idx == view->ap_sel);
        if (selected) {
            st7305_fill_rect(16, y - 4, 368, 30, true);
        }
        char line[48];
        snprintf(line, sizeof(line), "%s", view->aps[idx].ssid);
        st7305_draw_utf8(24, y, 1, line, !selected);
        char rssi[8];
        snprintf(rssi, sizeof(rssi), "%d", view->aps[idx].rssi);
        st7305_draw_utf8(376 - st7305_utf8_width(1, rssi), y, 1, rssi, !selected);
    }
    st7305_draw_utf8(20, 282, 1, "KEY选择 BOOT确认", true);
}

/* 网格行分段：数字10 / 大写13+13 / 小写13+13 / 符号16 / 虚拟项2(删 成)，
 * 分段必须与 WIFI_SETUP_CHARSET 的组成一致，合计 == WIFI_SETUP_SEL_COUNT */
static const uint8_t GRID_ROW_ITEMS[] = { 10, 13, 13, 13, 13, 16, 2 };
#define GRID_ROWS    ((int)(sizeof(GRID_ROW_ITEMS) / sizeof(GRID_ROW_ITEMS[0])))
#define GRID_X       20
#define GRID_Y       104
#define GRID_COL_W   22
#define GRID_ROW_H   24

static void draw_grid_item(int idx, int x, int y, bool selected)
{
    if (selected) {
        st7305_fill_rect(x + 1, y, 20, 22, true);
    }
    if (idx < WIFI_SETUP_CHARSET_LEN) {
        char ch[2] = { WIFI_SETUP_CHARSET[idx], '\0' };
        st7305_draw_utf8(x + (GRID_COL_W - 8) / 2, y + 3, 1, ch, !selected);
    } else {
        const char *label = (idx == WIFI_SETUP_SEL_DEL) ? "删" : "成";
        st7305_draw_utf8(x + (GRID_COL_W - 16) / 2, y + 3, 1, label, !selected);
    }
}

/* 整个字符集一次铺全，当前候选反白；候选序是一维的，行分段仅为排版 */
static void draw_charset_grid(int candidate)
{
    int idx = 0;
    for (int row = 0; row < GRID_ROWS; row++) {
        int y = GRID_Y + row * GRID_ROW_H;
        for (int col = 0; col < GRID_ROW_ITEMS[row]; col++) {
            draw_grid_item(idx, GRID_X + col * GRID_COL_W, y, idx == candidate);
            idx++;
        }
    }
}

static void draw_password(const sys_wifi_setup_view_t *view)
{
    st7305_draw_utf8(20, 46, 1, view->ssid, true);

    const char *pwd = view->pwd;
    int len = view->pwd_len;
    if (len > PWD_SHOW_MAX) {
        st7305_draw_utf8(20, 80, 1, "..", true);
        pwd += len - PWD_SHOW_MAX;
        st7305_draw_utf8(36, 80, 1, pwd, true);
    } else {
        st7305_draw_utf8(20, 80, 1, len > 0 ? pwd : "(空)", true);
    }
    /* 光标下划线指示输入位置 */
    int cursor_x = 20 + (len > PWD_SHOW_MAX ? 16 + PWD_SHOW_MAX : len) * 8;
    st7305_draw_line(cursor_x, 100, cursor_x + 7, 100, true);

    draw_charset_grid(view->pwd_candidate);
    /* 底部提示行与"密码至少8位"互斥：位数不足时警告优先 */
    if (len > 0 && len < 8) {
        st7305_draw_utf8(20, 282, 1, "密码至少8位", true);
    } else {
        st7305_draw_utf8(20, 282, 1, "KEY选择 BOOT确认 长按删", true);
    }
}

static void draw(const sys_state_snapshot_t *snap)
{
    st7305_clear(false);
    ui_v2_draw_header("WIFI", snap);

    const sys_wifi_setup_view_t *view = &snap->wifi_setup;
    switch (view->state) {
    case SYS_WIFI_SETUP_OFF:
        draw_off(snap);
        break;
    case SYS_WIFI_SETUP_SCANNING:
        ui_v2_draw_empty_state(140, "扫描中", NULL);
        break;
    case SYS_WIFI_SETUP_AP_LIST:
        draw_ap_list(view);
        break;
    case SYS_WIFI_SETUP_PASSWORD:
        draw_password(view);
        break;
    case SYS_WIFI_SETUP_CONNECTING:
        ui_v2_draw_empty_state(140, "连接中", view->ssid);
        break;
    case SYS_WIFI_SETUP_FAILED:
        ui_v2_draw_empty_state(140, "连接失败", "BOOT重输 KEY返回");
        break;
    default:
        break;
    }

    esp_err_t err = st7305_flush();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "flush失败: %s", esp_err_to_name(err));
        return;
    }
    s_last_version = snap->version;
}

static void on_enter(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);
    s_last_version = 0;
    draw(&snap);
}

static void on_tick(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);
    if (snap.version != s_last_version) {
        draw(&snap);
    }
}

static sys_wifi_cmd_t key_to_cmd(ui_key_event_t evt, bool setup_active)
{
    switch (evt) {
    case UI_KEY_NEXT:
        return SYS_WIFI_CMD_NEXT;
    case UI_KEY_PREV:
        return SYS_WIFI_CMD_PREV;
    case UI_KEY_ACTION:
        return setup_active ? SYS_WIFI_CMD_CONFIRM : SYS_WIFI_CMD_ENTER;
    case UI_KEY_BACK:
        return setup_active ? SYS_WIFI_CMD_BACK : SYS_WIFI_CMD_NONE;
    default:
        return SYS_WIFI_CMD_NONE;
    }
}

static bool on_key(ui_key_event_t evt)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);
    bool setup_active = (snap.wifi_setup.state != SYS_WIFI_SETUP_OFF);
    if (!setup_active && evt != UI_KEY_ACTION) {
        return false; /* OFF态只接管BOOT短按，其余交回全局翻页/取消 */
    }
    sys_wifi_cmd_t cmd = key_to_cmd(evt, setup_active);
    if (cmd != SYS_WIFI_CMD_NONE) {
        sys_state_post_wifi_cmd(cmd);
    }
    return true;
}

static const ui_page_t s_page = {
    .name = "wifi",
    .on_enter = on_enter,
    .on_exit = NULL,
    .on_tick = on_tick,
    .on_key = on_key,
};

const ui_page_t *page_wifi_get(void)
{
    return &s_page;
}
