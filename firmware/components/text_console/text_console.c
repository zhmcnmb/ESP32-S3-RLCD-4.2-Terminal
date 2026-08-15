/* text_console.c - USB-Serial-JTAG 行读取 -> sys_state 文本邮箱。
 * Type-C 直连 ESP32-S3 原生 USB Serial/JTAG(烧录/日志同一口)，RX 无其他消费者。 */
#include "text_console.h"

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sys_state.h"

static const char *TAG = "text_console";

static void text_console_task(void *arg)
{
    (void)arg;
    /* 静态行缓冲：单任务独占，避免栈占用与动态分配 */
    static char s_line[SYS_STATE_TEXT_TURN_MAX];
    size_t len = 0;
    for (;;) {
        uint8_t ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch != '\n') {
            if (len < sizeof(s_line) - 1) {
                s_line[len++] = (char)ch;
            } /* 超长部分静默丢弃，post 时整体过长也会拒收 */
            continue;
        }
        if (len == 0) {
            continue;
        }
        s_line[len] = '\0';
        len = 0;
        if (!sys_state_post_text_turn(s_line)) {
            ESP_LOGW(TAG, "文本邮箱占槽或过长，丢弃");
        }
    }
}

esp_err_t text_console_init(void)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        esp_err_t err = usb_serial_jtag_driver_install(&cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "USB-Serial-JTAG 驱动安装失败: %s", esp_err_to_name(err));
            return err;
        }
    }
    BaseType_t ok = xTaskCreate(text_console_task, "text_console", 3072, NULL, 2, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "串口文本控制台就绪(回车发送一行)");
    return ESP_OK;
}
