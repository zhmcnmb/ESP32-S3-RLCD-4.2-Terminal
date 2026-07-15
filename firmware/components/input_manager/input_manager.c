#include "input_manager.h"

#include "board_rlcd42.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "input";

#define POLL_INTERVAL_MS 30
#define LONG_PRESS_MS    1000

static input_event_cb_t s_handler = NULL;

static void emit(input_event_t evt)
{
    if (s_handler) {
        s_handler(evt);
    }
}

/* KEY/BOOT两键消抖与长短按判定逻辑完全对称：
 * 持续按下累计时长，到阈值前松开算短按(NEXT/ACTION)，到阈值那一刻立即触发长按
 * (VOICE/BACK)并置位，之后即便继续按住或松开都不再补发短按 */
static void poll_gpio(void)
{
    static bool key_prev = false;
    static int key_held_ms = 0;
    static bool key_long_fired = false;

    static bool boot_prev = false;
    static int boot_held_ms = 0;
    static bool boot_long_fired = false;

    bool key = board_btn_key_pressed();
    if (key) {
        key_held_ms += POLL_INTERVAL_MS;
        if (!key_long_fired && key_held_ms >= LONG_PRESS_MS) {
            key_long_fired = true;
            emit(INPUT_EVENT_VOICE);
        }
    } else {
        if (key_prev && !key_long_fired) {
            emit(INPUT_EVENT_NEXT);
        }
        key_held_ms = 0;
        key_long_fired = false;
    }
    key_prev = key;

    bool boot = board_btn_boot_pressed();
    if (boot) {
        boot_held_ms += POLL_INTERVAL_MS;
        if (!boot_long_fired && boot_held_ms >= LONG_PRESS_MS) {
            boot_long_fired = true;
            emit(INPUT_EVENT_BACK);
        }
    } else {
        if (boot_prev && !boot_long_fired) {
            emit(INPUT_EVENT_ACTION);
        }
        boot_held_ms = 0;
        boot_long_fired = false;
    }
    boot_prev = boot;
}

static void input_task(void *arg)
{
    (void)arg;
    for (;;) {
        poll_gpio();
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

esp_err_t input_manager_init(void)
{
    BaseType_t ok = xTaskCreate(input_task, "input_task", 2048, NULL, 4, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");
    return ESP_OK;
}

esp_err_t input_manager_set_handler(input_event_cb_t cb)
{
    s_handler = cb;
    return ESP_OK;
}
