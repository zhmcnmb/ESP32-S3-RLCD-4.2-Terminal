#include "ui_manager.h"

#include "display_st7305.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "input_manager.h"

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

/* 语音功能R0阶段未接入(P5实现)：仅在页脚区画一条短暂提示，不改变当前页面的
 * 状态或数据。提示会在下一次周期性on_tick(<=1s后)被当前页整屏重绘自然覆盖，
 * 不需要额外的定时器或恢复逻辑 */
static void show_voice_placeholder(void)
{
    st7305_fill_rect(20, 266, 360, 34, false);
    st7305_fill_rect(20, 266, 360, 1, true);
    const char *msg = "语音功能待接入";
    st7305_draw_utf8((400 - st7305_utf8_width(1, msg)) / 2, 280, 1, msg, true);
    esp_err_t err = st7305_flush();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "语音提示flush失败: %s", esp_err_to_name(err));
    }
}

/* 把 input_manager 的语义事件映射为页面动作。所有实际绘制(含上面的语音提示)
 * 都只在 ui_task 这一个任务里发生，避免和周期性 on_tick 竞争帧缓冲/SPI */
static void dispatch_input(input_event_t evt)
{
    switch (evt) {
    case INPUT_EVENT_NEXT:
        dispatch_key(UI_KEY_NEXT);
        break;
    case INPUT_EVENT_ACTION:
        dispatch_key(UI_KEY_ACTION);
        break;
    case INPUT_EVENT_BACK:
        dispatch_key(UI_KEY_BACK);
        break;
    case INPUT_EVENT_VOICE:
        show_voice_placeholder();
        break;
    }
}

/* input_manager 的回调只做入队，绝不在 input_task 上下文里直接绘制
 * (r0-ui-v2-spec.md 第4节: 输入状态变化不得直接调用页面绘制) */
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

    return input_manager_set_handler(on_input_event);
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
