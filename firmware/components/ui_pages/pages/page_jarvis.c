#include <string.h>

#include "display_st7305.h"
#include "esp_log.h"
#include "sys_state.h"
#include "ui_pages.h"
#include "ui_v2.h"

/*
 * Jarvis 默认页：圆形双眼头像 + 极短状态。
 * 只读 sys_state 快照；全局语音取消由 ui_manager 提交。
 * 以最少几何区分相位，默认状态保持安静。
 */

static const char *TAG = "page_jarvis";

typedef struct {
    sys_voice_phase_t phase;
    bool wifi_connected;
    bool sd_mounted;
    bool battery_ok;
    int battery_percent;
    bool battery_charging;
    char voice_error[24];
} jarvis_view_t;

static jarvis_view_t s_view;
static bool s_drawn;

/* 状态短文案：固定字面量，完整ASR/LLM文本不在此显示。ERROR 相位读取
 * sys_state 里 voice_assistant 写入的短原因(如"未听清")，没有则显示通用"故障" */
static const char *phase_label(const sys_state_snapshot_t *snap)
{
    sys_voice_phase_t phase = snap->voice_phase;
    if (!snap->wifi_connected && (phase == SYS_VOICE_IDLE || phase == SYS_VOICE_ERROR)) {
        return "离线";
    }
    switch (phase) {
    case SYS_VOICE_LISTENING:
        return "聆听";
    case SYS_VOICE_TRANSCRIBING:
        return "转写";
    case SYS_VOICE_THINKING:
        return "思考";
    case SYS_VOICE_EXECUTING:
        return "执行";
    case SYS_VOICE_SPEAKING:
        return "回答";
    case SYS_VOICE_CONFIRM:
        return "确认";
    case SYS_VOICE_ERROR:
        return snap->voice_error[0] ? snap->voice_error : "故障";
    case SYS_VOICE_IDLE:
    default:
        return NULL; /* 正常待命保持安静 */
    }
}

static bool view_changed(const sys_state_snapshot_t *snap)
{
    return !s_drawn || s_view.phase != snap->voice_phase
           || s_view.wifi_connected != snap->wifi_connected
           || s_view.sd_mounted != snap->sd_mounted
           || s_view.battery_ok != snap->battery_ok
           || s_view.battery_percent != snap->battery_percent
           || s_view.battery_charging != snap->battery_charging
           || strcmp(s_view.voice_error, snap->voice_error) != 0;
}

static void save_view(const sys_state_snapshot_t *snap)
{
    s_view.phase = snap->voice_phase;
    s_view.wifi_connected = snap->wifi_connected;
    s_view.sd_mounted = snap->sd_mounted;
    s_view.battery_ok = snap->battery_ok;
    s_view.battery_percent = snap->battery_percent;
    s_view.battery_charging = snap->battery_charging;
    strncpy(s_view.voice_error, snap->voice_error, sizeof(s_view.voice_error) - 1);
    s_view.voice_error[sizeof(s_view.voice_error) - 1] = '\0';
}

/* 圆环是头像唯一轮廓，两个眼睛承担大部分状态表达，避免机械面甲抢占状态信息。 */
static void draw_orb_ring(int cx, int cy)
{
    const int radius = 72;
    st7305_draw_round_rect(cx - radius, cy - radius, 2 * radius + 1, 2 * radius + 1,
                            radius, true);
}


/* 头像只在半径不超过16px时绘制实心眼睛，逐行填充比引入位图更节省资源。 */
static void draw_filled_disc(int cx, int cy, int radius)
{
    for (int y = -radius; y <= radius; y++) {
        int x = 0;
        while ((x + 1) * (x + 1) + y * y <= radius * radius) {
            x++;
        }
        st7305_fill_rect(cx - x, cy + y, 2 * x + 1, 1, true);
    }
}

static void draw_orb_eyes(int cx, int cy, sys_voice_phase_t phase, bool wifi_ok)
{
    int radius = phase == SYS_VOICE_LISTENING ? 13 : 10;
    if (phase == SYS_VOICE_SPEAKING) {
        radius = 12;
    } else if (phase == SYS_VOICE_THINKING || phase == SYS_VOICE_TRANSCRIBING) {
        radius = 7;
    }

    int eye_y = cy - 14;
    int left_x = cx - 30;
    int right_x = cx + 30;
    if (phase == SYS_VOICE_ERROR || !wifi_ok) {
        st7305_draw_line(left_x - radius, eye_y - radius, left_x + radius, eye_y + radius, true);
        st7305_draw_line(left_x + radius, eye_y - radius, left_x - radius, eye_y + radius, true);
        st7305_draw_line(right_x - radius, eye_y - radius, right_x + radius, eye_y + radius, true);
        st7305_draw_line(right_x + radius, eye_y - radius, right_x - radius, eye_y + radius, true);
        return;
    }

    draw_filled_disc(left_x, eye_y, radius);
    draw_filled_disc(right_x, eye_y, radius);
}

/* 下半圆弧形成连续微笑；逐列绘制避免折线转角和独立像素落在嘴部。 */
static void draw_orb_smile(int cx, int cy)
{
    const int radius = 29;
    const int center_y = cy + 7;

    for (int x = -24; x <= 24; x++) {
        int y = 0;
        while ((y + 1) * (y + 1) + x * x <= radius * radius) {
            y++;
        }
        st7305_fill_rect(cx + x, center_y + y, 1, 2, true);
    }
}

static void draw_orb_phase_mark(int cx, int cy, sys_voice_phase_t phase)
{
    int y = cy + 40;

    /* 聆听/转写相位靠眼睛尺寸与底部文字区分,嘴部留白避免碎点。 */
    switch (phase) {
    case SYS_VOICE_THINKING:
        st7305_fill_rect(cx - 14, y, 4, 4, true);
        st7305_fill_rect(cx - 2, y, 4, 4, true);
        st7305_fill_rect(cx + 10, y, 4, 4, true);
        break;
    case SYS_VOICE_EXECUTING:
        st7305_draw_round_rect(cx - 14, y, 28, 6, 3, true);
        break;
    case SYS_VOICE_SPEAKING:
        st7305_fill_rect(cx - 16, y + 1, 10, 3, true);
        st7305_fill_rect(cx + 6, y + 1, 10, 3, true);
        break;
    case SYS_VOICE_CONFIRM:
        st7305_draw_line(cx - 10, y + 2, cx - 2, y + 8, true);
        st7305_draw_line(cx - 2, y + 8, cx + 12, y - 4, true);
        break;
    default:
        break;
    }
}

static void draw_avatar(int cx, int cy, sys_voice_phase_t phase, bool wifi_ok)
{
    draw_orb_ring(cx, cy);
    draw_orb_eyes(cx, cy, phase, wifi_ok);
    if (phase == SYS_VOICE_IDLE && wifi_ok) {
        draw_orb_smile(cx, cy);
    } else {
        draw_orb_phase_mark(cx, cy, phase);
    }
}

static void draw(const sys_state_snapshot_t *snap)
{
    st7305_clear(false);
    ui_v2_draw_header("JARVIS", snap);
    draw_avatar(200, 158, snap->voice_phase, snap->wifi_connected);

    const char *label = phase_label(snap);
    if (label) {
        st7305_draw_utf8((400 - st7305_utf8_width(2, label)) / 2, 258, 2, label, true);
    }

    esp_err_t err = st7305_flush();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "flush失败: %s", esp_err_to_name(err));
        return;
    }
    save_view(snap);
    s_drawn = true;
}

static void on_enter(void)
{
    s_drawn = false;
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);
    draw(&snap);
}

static void on_tick(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);
    if (view_changed(&snap)) {
        draw(&snap);
    }
}

static const ui_page_t s_page = {
    .name = "jarvis",
    .on_enter = on_enter,
    .on_exit = NULL,
    .on_tick = on_tick,
    .on_key = NULL,
};

const ui_page_t *page_jarvis_get(void)
{
    return &s_page;
}
