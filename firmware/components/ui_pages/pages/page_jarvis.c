#include <stdio.h>
#include <string.h>

#include "display_st7305.h"
#include "esp_log.h"
#include "sys_state.h"
#include "ui_pages.h"
#include "ui_v2.h"

/*
 * Jarvis 默认页：专属机械面甲头像 + 极短状态。
 * 只读 sys_state 快照；全局语音取消由 ui_manager 提交。
 * 样式原则见 docs/voice-agent-design.md 第9节——不固定最终美术，只保证可辨识。
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

/* 面甲自身就是本页唯一结构轮廓：直接画八边形，不再用矩形外框叠斜线。
 * 相位只改变眼和核心，外形在状态切换时保持稳定，避免画面显得嘈杂。 */
static void draw_avatar(int cx, int cy, sys_voice_phase_t phase, bool wifi_ok)
{
    const int w = 180, h = 170, cut = 22;
    const int x = cx - w / 2;
    const int y = cy - h / 2;
    bool active = (phase != SYS_VOICE_IDLE) || !wifi_ok;

    st7305_draw_line(x + cut, y, x + w - cut, y, true);
    st7305_draw_line(x + w - cut, y, x + w, y + cut + 8, true);
    st7305_draw_line(x + w, y + cut + 8, x + w, y + h - cut - 8, true);
    st7305_draw_line(x + w, y + h - cut - 8, x + w - cut, y + h, true);
    st7305_draw_line(x + w - cut, y + h, x + cut, y + h, true);
    st7305_draw_line(x + cut, y + h, x, y + h - cut - 8, true);
    st7305_draw_line(x, y + h - cut - 8, x, y + cut + 8, true);
    st7305_draw_line(x, y + cut + 8, x + cut, y, true);

    /* 两条短接缝给面甲结构感；天线只做身份点缀，不再围出第二层框。 */
    st7305_draw_line(x + 32, y + 40, x + w - 32, y + 40, true);
    st7305_draw_line(x + 32, y + h - 10, x + w - 32, y + h - 10, true);
    st7305_fill_rect(x + 16, y - 8, 2, 8, true);
    st7305_fill_rect(x + w - 18, y - 8, 2, 8, true);

    int eye_y = y + 60;
    int eye_w = phase == SYS_VOICE_LISTENING ? 30 : 24;
    int eye_h = (phase == SYS_VOICE_THINKING || phase == SYS_VOICE_TRANSCRIBING) ? 4 : 11;
    int left_x = cx - 44 - eye_w / 2;
    int right_x = cx + 44 - eye_w / 2;
    if (phase == SYS_VOICE_ERROR || !wifi_ok) {
        st7305_draw_line(left_x, eye_y, left_x + eye_w, eye_y + eye_h, true);
        st7305_draw_line(left_x + eye_w, eye_y, left_x, eye_y + eye_h, true);
        st7305_draw_line(right_x, eye_y, right_x + eye_w, eye_y + eye_h, true);
        st7305_draw_line(right_x + eye_w, eye_y, right_x, eye_y + eye_h, true);
    } else if (active) {
        st7305_fill_rect(left_x, eye_y, eye_w, eye_h, true);
        st7305_fill_rect(right_x, eye_y, eye_w, eye_h, true);
    } else {
        st7305_draw_rect(left_x, eye_y, eye_w, eye_h, 1, true);
        st7305_draw_rect(right_x, eye_y, eye_w, eye_h, 1, true);
    }

    int core = (phase == SYS_VOICE_SPEAKING) ? 22 : 16;
    int core_x = cx - core / 2;
    int core_y = y + 104;
    if (phase == SYS_VOICE_SPEAKING || phase == SYS_VOICE_LISTENING) {
        st7305_fill_rect(core_x, core_y, core, core, true);
    } else {
        st7305_draw_rect(core_x, core_y, core, core, 2, true);
    }
    st7305_fill_rect(core_x - 14, core_y + core / 2 - 1, 8, 2, true);
    st7305_fill_rect(core_x + core + 6, core_y + core / 2 - 1, 8, 2, true);
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
