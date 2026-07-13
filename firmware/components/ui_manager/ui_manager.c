#include "ui_manager.h"

#include "board_rlcd42.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ui";

#define POLL_INTERVAL_MS 30
#define TICK_INTERVAL_MS 1000
#define LONG_PRESS_MS    1000

static const ui_page_t *s_pages[UI_MAX_PAGES];
static int s_page_count = 0;
static int s_current = -1;

static void enter_page(int index)
{
    if (index < 0 || index >= s_page_count) {
        return;
    }
    if (s_current >= 0 && s_pages[s_current]->on_exit) {
        s_pages[s_current]->on_exit();
    }
    s_current = index;
    ESP_LOGI(TAG, "enter page: %s", s_pages[s_current]->name);
    if (s_pages[s_current]->on_enter) {
        s_pages[s_current]->on_enter();
    }
    if (s_pages[s_current]->on_tick) {
        s_pages[s_current]->on_tick();
    }
}

static void dispatch_key(ui_key_event_t evt)
{
    if (s_current < 0) {
        return;
    }
    const ui_page_t *page = s_pages[s_current];
    if (page->on_key && page->on_key(evt)) {
        return; /* 页面已消费 */
    }
    if (evt == UI_KEY_NEXT) {
        enter_page((s_current + 1) % s_page_count);
    }
}

/* 按键消抖与长短按判定: KEY只产生NEXT; BOOT短按ACTION、长按BACK */
static void poll_keys(void)
{
    static bool key_prev = false;
    static bool boot_prev = false;
    static int boot_held_ms = 0;
    static bool boot_long_fired = false;

    bool key = board_btn_key_pressed();
    if (key && !key_prev) {
        dispatch_key(UI_KEY_NEXT);
    }
    key_prev = key;

    bool boot = board_btn_boot_pressed();
    if (boot) {
        boot_held_ms += POLL_INTERVAL_MS;
        if (!boot_long_fired && boot_held_ms >= LONG_PRESS_MS) {
            boot_long_fired = true;
            dispatch_key(UI_KEY_BACK);
        }
    } else {
        if (boot_prev && !boot_long_fired) {
            dispatch_key(UI_KEY_ACTION); /* 抬手时才判定为短按 */
        }
        boot_held_ms = 0;
        boot_long_fired = false;
    }
    boot_prev = boot;
}

static void ui_task(void *arg)
{
    int elapsed = 0;
    for (;;) {
        poll_keys();

        elapsed += POLL_INTERVAL_MS;
        if (elapsed >= TICK_INTERVAL_MS) {
            elapsed = 0;
            if (s_current >= 0 && s_pages[s_current]->on_tick) {
                s_pages[s_current]->on_tick();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

esp_err_t ui_manager_init(void)
{
    s_page_count = 0;
    s_current = -1;
    return ESP_OK;
}

esp_err_t ui_manager_register_page(const ui_page_t *page)
{
    ESP_RETURN_ON_FALSE(page && page->name, ESP_ERR_INVALID_ARG, TAG, "bad page");
    ESP_RETURN_ON_FALSE(s_page_count < UI_MAX_PAGES, ESP_ERR_NO_MEM, TAG, "too many pages");

    s_pages[s_page_count++] = page;
    ESP_LOGI(TAG, "registered page: %s", page->name);
    return ESP_OK;
}

esp_err_t ui_manager_start(void)
{
    ESP_RETURN_ON_FALSE(s_page_count > 0, ESP_ERR_INVALID_STATE, TAG, "no pages");

    enter_page(0);
    BaseType_t ok = xTaskCreate(ui_task, "ui_task", 4096, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");
    return ESP_OK;
}

void ui_manager_goto(int index)
{
    enter_page(index);
}
