#include "input_manager.h"

#include "board_rlcd42.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "input";

#define POLL_INTERVAL_MS 30
#define LONG_PRESS_MS    1000
#define DEBOUNCE_STABLE_SAMPLES 3

static input_event_cb_t s_handler = NULL;

typedef struct {
    bool raw_pressed;
    bool pressed;
    uint8_t stable_samples;
    int held_ms;
    bool long_fired;
} button_state_t;

static void emit(input_event_t evt)
{
    if (s_handler) {
        s_handler(evt);
    }
}

static bool update_button(button_state_t *state, bool raw_pressed)
{
    if (raw_pressed != state->raw_pressed) {
        state->raw_pressed = raw_pressed;
        state->stable_samples = 1;
        return false;
    }
    if (state->stable_samples < DEBOUNCE_STABLE_SAMPLES) {
        state->stable_samples++;
    }
    if (state->stable_samples != DEBOUNCE_STABLE_SAMPLES || state->pressed == raw_pressed) {
        return false;
    }
    state->pressed = raw_pressed;
    return true;
}

/* KEY短按 NEXT、长按 PREV；BOOT 短按 ACTION、长按 BACK。 */
static void poll_gpio(void)
{
    static button_state_t key;
    static button_state_t boot;

    bool key_changed = update_button(&key, board_btn_key_pressed());
    if (key.pressed) {
        key.held_ms += POLL_INTERVAL_MS;
        if (!key.long_fired && key.held_ms >= LONG_PRESS_MS) {
            key.long_fired = true;
            emit(INPUT_EVENT_PREV);
        }
    } else if (key_changed) {
        if (!key.long_fired) {
            emit(INPUT_EVENT_NEXT);
        }
        key.held_ms = 0;
        key.long_fired = false;
    }

    bool boot_changed = update_button(&boot, board_btn_boot_pressed());
    if (boot.pressed) {
        boot.held_ms += POLL_INTERVAL_MS;
        if (!boot.long_fired && boot.held_ms >= LONG_PRESS_MS) {
            boot.long_fired = true;
            emit(INPUT_EVENT_BACK);
        }
    } else if (boot_changed) {
        if (!boot.long_fired) {
            emit(INPUT_EVENT_ACTION);
        }
        boot.held_ms = 0;
        boot.long_fired = false;
    }
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
