#include <stdio.h>

#include "board_rlcd42.h"
#include "display_st7305.h"
#include "esp_log.h"
#include "storage_sd.h"
#include "ui_pages.h"

/*
 * 诊断页: 显示硬件自检结果。
 * 反面参照——此页绕过 sys_state 直接读硬件（I2C/SD卡），使用存储层头文件，
 * 不符合"页面不准自己发HTTP/读传感器，数据从服务层取"的契约。新增页面切勿模仿。
 * 保留作为旧版诊断入口，不注册进翻页。
 */

static const char *TAG = "page_diag";

static int s_i2c_found = 0;
static bool s_i2c_ok[4];

static void draw(void)
{
    st7305_clear(false);

    /* 边框: 验证四边无偏移 */
    st7305_fill_rect(0, 0, 400, 2, true);
    st7305_fill_rect(0, 298, 400, 2, true);
    st7305_fill_rect(0, 0, 2, 300, true);
    st7305_fill_rect(398, 0, 2, 300, true);

    st7305_draw_text(28, 24, 3, "ESP32-S3 RLCD 4.2", true);
    st7305_draw_text(28, 64, 3, "PHASE 1 OK", true);

    char line[64];
    snprintf(line, sizeof(line), "I2C DEVICES: %d/4", s_i2c_found);
    st7305_draw_text(28, 108, 2, line, true);

    const char *names[4] = { "SHTC3", "ES8311", "ES7210", "PCF85063" };
    int y = 136;
    for (int i = 0; i < 4; i++) {
        snprintf(line, sizeof(line), "%s %s", s_i2c_ok[i] ? "OK" : "--", names[i]);
        st7305_draw_text(28, y, 2, line, true);
        y += 22;
    }

    /* SD卡状态 */
    if (storage_sd_is_mounted()) {
        storage_sd_info_t info;
        if (storage_sd_get_info(&info) == ESP_OK) {
            snprintf(line, sizeof(line), "SD %lluMB / %lluMB",
                     info.used_bytes / (1024 * 1024), info.total_bytes / (1024 * 1024));
        } else {
            snprintf(line, sizeof(line), "SD MOUNTED");
        }
    } else {
        snprintf(line, sizeof(line), "SD NOT FOUND");
    }
    st7305_draw_text(28, 228, 2, line, true);

    st7305_draw_text(28, 258, 2, "KEY:NEXT  BOOT:RESCAN", true);

    esp_err_t err = st7305_flush();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "flush failed: %s", esp_err_to_name(err));
    }
}

static void on_enter(void)
{
    s_i2c_found = board_i2c_scan(&s_i2c_ok[0], &s_i2c_ok[1], &s_i2c_ok[2], &s_i2c_ok[3]);
    draw();
}

static bool on_key(ui_key_event_t evt)
{
    if (evt == UI_KEY_ACTION) {
        ESP_LOGI(TAG, "rescan I2C");
        on_enter(); /* 重新扫描并重绘 */
        return true;
    }
    return false; /* NEXT交回ui_manager切页 */
}

static const ui_page_t s_page = {
    .name = "diag",
    .on_enter = on_enter,
    .on_exit = NULL,
    .on_tick = NULL, /* 静态内容，无需周期刷新 */
    .on_key = on_key,
};

const ui_page_t *page_diag_get(void)
{
    return &s_page;
}
