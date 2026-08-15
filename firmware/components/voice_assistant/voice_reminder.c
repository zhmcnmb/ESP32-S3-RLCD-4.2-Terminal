/* voice_reminder.c - 空闲提醒的播报。
 * PTT-only下空闲时录音管线本就停止，提醒直接播报，无需抢占/恢复麦克风。 */
#include "voice_assistant_internal.h"

#include <stdio.h>

#include "esp_log.h"
#include "sys_state.h"

static const char *TAG = "voice_reminder";

void va_reminder_play(va_reminder_transition_cb_t on_started,
                      va_reminder_transition_cb_t on_finished,
                      const volatile bool *cancel_flag)
{
    if (!on_started || !on_finished || !sys_state_reminder_pending()) return;
    char text[SYS_STATE_REMINDER_TEXT_MAX];
    if (!sys_state_consume_reminder(text, sizeof(text))) {
        ESP_LOGW(TAG, "提醒邮箱在播报前变为空");
        return;
    }
    on_started();
    char speech[SYS_STATE_REMINDER_TEXT_MAX + 10];
    snprintf(speech, sizeof(speech), "提醒" "\xEF\xBC\x9A%s", text);
    int status = 0;
    esp_err_t err = va_tts_play_clean(speech, cancel_flag, &status);
    if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
        ESP_LOGW(TAG, "提醒播报失败: %s status=%d", esp_err_to_name(err), status);
    }
    on_finished();
}
