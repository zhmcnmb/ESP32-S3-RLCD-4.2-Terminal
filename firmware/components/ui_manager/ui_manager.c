#include "ui_manager.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "input_manager.h"
#include "sys_state.h"

static const char *TAG = "ui";

#define TICK_INTERVAL_MS 1000
#define INPUT_QUEUE_LEN  4

static const ui_page_t *s_pages[UI_MAX_PAGES];
static int s_page_count = 0;
static int s_current = -1;
static QueueHandle_t s_input_queue;

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
    /* 只调用 on_enter 画首帧；禁止立刻再调 on_tick 造成双 flush */
    if (s_pages[s_current]->on_enter) {
        s_pages[s_current]->on_enter();
    }
}

static bool cancel_active_voice(ui_key_event_t evt)
{
    if (evt != UI_KEY_BACK) {
        return false;
    }
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);
    if (snap.voice_phase == SYS_VOICE_IDLE) {
        return false;
    }
    sys_state_request_voice_cancel();
    return true;
}

static void trigger_idle_voice(void)
{
    sys_state_snapshot_t snap;
    sys_state_get_snapshot(&snap);
    if (snap.voice_phase == SYS_VOICE_IDLE) {
        sys_state_request_voice_trigger();
    }
}

static void dispatch_key(ui_key_event_t evt)
{
    if (s_current < 0) {
        return;
    }
    if (cancel_active_voice(evt)) {
        return;
    }
    const ui_page_t *page = s_pages[s_current];
    if (page->on_key && page->on_key(evt)) {
        return; /* 页面已消费 */
    }
    if (evt == UI_KEY_NEXT) {
        enter_page((s_current + 1) % s_page_count);
    } else if (evt == UI_KEY_PREV) {
        enter_page((s_current - 1 + s_page_count) % s_page_count);
    } else if (evt == UI_KEY_ACTION) {
        trigger_idle_voice(); /* BOOT短按: 空闲时手动触发语音 */
    }
}

/* 把 input_manager 的语义事件映射为页面动作。绘制只在 ui_task 发生，
 * 避免和周期性 on_tick 竞争帧缓冲/SPI。未接入功能不产生占位 UI。 */
static void dispatch_input(input_event_t evt)
{
    switch (evt) {
    case INPUT_EVENT_NEXT:
        dispatch_key(UI_KEY_NEXT);
        break;
    case INPUT_EVENT_PREV:
        dispatch_key(UI_KEY_PREV);
        break;
    case INPUT_EVENT_ACTION:
        dispatch_key(UI_KEY_ACTION);
        break;
    case INPUT_EVENT_BACK:
        dispatch_key(UI_KEY_BACK);
        break;
    }
}

/* input_manager 的回调只做入队，绝不在 input_task 上下文里直接绘制，
 * 避免多个任务同时操作帧缓冲和 SPI。 */
static void on_input_event(input_event_t evt)
{
    if (xQueueSend(s_input_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "input queue full, dropped event %d", evt);
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    for (;;) {
        input_event_t evt;
        if (xQueueReceive(s_input_queue, &evt, pdMS_TO_TICKS(TICK_INTERVAL_MS)) == pdTRUE) {
            dispatch_input(evt);
        } else if (s_current >= 0 && s_pages[s_current]->on_tick) {
            s_pages[s_current]->on_tick();
        }
    }
}

esp_err_t ui_manager_init(void)
{
    s_page_count = 0;
    s_current = -1;

    s_input_queue = xQueueCreate(INPUT_QUEUE_LEN, sizeof(input_event_t));
    ESP_RETURN_ON_FALSE(s_input_queue, ESP_ERR_NO_MEM, TAG, "input queue");

    esp_err_t err = input_manager_set_handler(on_input_event);
    if (err != ESP_OK) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return err;
    }
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

    BaseType_t ok = xTaskCreate(ui_task, "ui_task", 4096, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");
    enter_page(0);
    return ESP_OK;
}

