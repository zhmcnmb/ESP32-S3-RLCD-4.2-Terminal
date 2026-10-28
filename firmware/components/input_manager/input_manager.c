#include "input_manager.h"

#include "board_rlcd42.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "input";

#define POLL_INTERVAL_MS 30
#define LONG_PRESS_MS    1000
#define DEBOUNCE_STABLE_SAMPLES 3

static input_event_cb_t s_handler = NULL;
static SemaphoreHandle_t s_press_sem = NULL;

/* 消抖状态提到文件作用域：input_task 的阻塞唤醒条件需要读取两键状态 */
typedef struct {
    bool raw_pressed;
    bool pressed;
    uint8_t stable_samples;
    int held_ms;
    bool long_fired;
} button_state_t;

static button_state_t s_key;
static button_state_t s_boot;

/* 按键负沿中断：ISR只发信号量唤醒任务，消抖与长短按判定仍按30ms步进
 * 在任务上下文完成（与原轮询语义一致）。ISR里不读电平，避免抖动期误判 */
static void IRAM_ATTR button_isr(void *arg)
{
    (void)arg;
    BaseType_t higher = pdFALSE;
    xSemaphoreGiveFromISR(s_press_sem, &higher);
    if (higher) {
        portYIELD_FROM_ISR();
    }
}

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
    bool key_changed = update_button(&s_key, board_btn_key_pressed());
    if (s_key.pressed) {
        s_key.held_ms += POLL_INTERVAL_MS;
        if (!s_key.long_fired && s_key.held_ms >= LONG_PRESS_MS) {
            s_key.long_fired = true;
            emit(INPUT_EVENT_PREV);
        }
    } else if (key_changed) {
        if (!s_key.long_fired) {
            emit(INPUT_EVENT_NEXT);
        }
        s_key.held_ms = 0;
        s_key.long_fired = false;
    }

    bool boot_changed = update_button(&s_boot, board_btn_boot_pressed());
    if (s_boot.pressed) {
        s_boot.held_ms += POLL_INTERVAL_MS;
        if (!s_boot.long_fired && s_boot.held_ms >= LONG_PRESS_MS) {
            s_boot.long_fired = true;
            emit(INPUT_EVENT_BACK);
        }
    } else if (boot_changed) {
        if (!s_boot.long_fired) {
            emit(INPUT_EVENT_ACTION);
        }
        s_boot.held_ms = 0;
        s_boot.long_fired = false;
    }
}

static void input_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* 平时深度阻塞在信号量上（不再30ms轮询，给tickless浅睡让出长空闲窗）。
         * 按下后沿用原30ms步进消抖+长短按判定，直到两键都确认释放才回阻塞；
         * 处理期间的新沿会重复给信号量，由释放后的排空拿掉，不产生多余事件 */
        xSemaphoreTake(s_press_sem, portMAX_DELAY);
        do {
            poll_gpio();
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        } while (s_key.pressed || s_boot.pressed);
        xSemaphoreTake(s_press_sem, 0); /* 排空释放抖动期累积的信号量 */
    }
}

/* 中断资源逆序回收，任务创建失败时保持与未初始化等价的状态 */
static void isr_teardown(void)
{
    gpio_set_intr_type(BOARD_BTN_BOOT_PIN, GPIO_INTR_DISABLE);
    gpio_set_intr_type(BOARD_BTN_KEY_PIN, GPIO_INTR_DISABLE);
    gpio_isr_handler_remove(BOARD_BTN_BOOT_PIN);
    gpio_isr_handler_remove(BOARD_BTN_KEY_PIN);
    gpio_uninstall_isr_service();
    vSemaphoreDelete(s_press_sem);
    s_press_sem = NULL;
}

esp_err_t input_manager_init(void)
{
    s_press_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_press_sem, ESP_ERR_NO_MEM, TAG, "sem create");

    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(BOARD_BTN_BOOT_PIN, button_isr, NULL);
    }
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(BOARD_BTN_KEY_PIN, button_isr, NULL);
    }
    /* 先挂handler再开中断沿，避免注册过程中的悬空触发 */
    if (err == ESP_OK) {
        err = gpio_set_intr_type(BOARD_BTN_BOOT_PIN, GPIO_INTR_NEGEDGE);
    }
    if (err == ESP_OK) {
        err = gpio_set_intr_type(BOARD_BTN_KEY_PIN, GPIO_INTR_NEGEDGE);
    }
    if (err != ESP_OK) {
        isr_teardown();
        ESP_RETURN_ON_ERROR(err, TAG, "isr setup");
    }

    /* 为后续 tickless 浅睡铺路：GPIO低电平唤醒须在开启PM前注册好。
     * 失败只降级（开PM前无影响），不阻断按键功能 */
    esp_err_t wake_err = gpio_wakeup_enable(BOARD_BTN_BOOT_PIN, GPIO_INTR_LOW_LEVEL);
    if (wake_err == ESP_OK) {
        wake_err = gpio_wakeup_enable(BOARD_BTN_KEY_PIN, GPIO_INTR_LOW_LEVEL);
    }
    if (wake_err == ESP_OK) {
        wake_err = esp_sleep_enable_gpio_wakeup();
    }
    if (wake_err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO浅睡唤醒注册失败: %s（开启PM前需解决）", esp_err_to_name(wake_err));
    }

    BaseType_t ok = xTaskCreate(input_task, "input_task", 2048, NULL, 4, NULL);
    if (ok != pdPASS) {
        isr_teardown();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t input_manager_set_handler(input_event_cb_t cb)
{
    s_handler = cb;
    return ESP_OK;
}
