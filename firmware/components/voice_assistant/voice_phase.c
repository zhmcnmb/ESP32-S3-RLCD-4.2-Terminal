/* voice_phase.c - 语音相位状态机：转换合法性校验 + sys_state 推送。
 * 从 voice_assistant 抽出，使状态转换规则集中可独立审查/纯测。
 * 状态机：只允许语义合理转换；非法转换拒绝并记录。 */
#include "voice_phase.h"

#include "esp_log.h"

static const char *TAG = "voice_phase";
static sys_voice_phase_t s_phase = SYS_VOICE_IDLE;
static uint32_t s_turn_id;

static bool phase_transition_valid(sys_voice_phase_t from, sys_voice_phase_t to)
{
    if (from == to) {
        return true;
    }
    switch (from) {
    case SYS_VOICE_IDLE:
        /* scheduler 可在没有用户轮次时直接播报提醒；
         * 串口文本注入的测试轮跳过 LISTENING/TRANSCRIBING 直接进 THINKING。 */
        return to == SYS_VOICE_LISTENING || to == SYS_VOICE_SPEAKING || to == SYS_VOICE_THINKING;
    case SYS_VOICE_LISTENING:
        return to == SYS_VOICE_TRANSCRIBING || to == SYS_VOICE_IDLE || to == SYS_VOICE_ERROR;
    case SYS_VOICE_TRANSCRIBING:
        return to == SYS_VOICE_THINKING || to == SYS_VOICE_IDLE || to == SYS_VOICE_ERROR;
    case SYS_VOICE_THINKING:
        return to == SYS_VOICE_SPEAKING || to == SYS_VOICE_EXECUTING
            || to == SYS_VOICE_IDLE || to == SYS_VOICE_ERROR;
    case SYS_VOICE_EXECUTING:
        return to == SYS_VOICE_SPEAKING || to == SYS_VOICE_THINKING
            || to == SYS_VOICE_IDLE || to == SYS_VOICE_ERROR;
    case SYS_VOICE_SPEAKING:
        return to == SYS_VOICE_IDLE || to == SYS_VOICE_ERROR;
    case SYS_VOICE_ERROR:
        return to == SYS_VOICE_IDLE;
    default:
        return false;
    }
}

void voice_phase_set_turn(uint32_t turn_id)
{
    s_turn_id = turn_id;
}

sys_voice_phase_t voice_phase_get(void)
{
    return s_phase;
}

/* 复合状态更新：校验转换合法性 + 推进相位 + (仅ERROR)带短原因文案，
 * page_jarvis 据此区分"网络故障"/"未听清"等，而不是所有异常显示同一个词 */
void voice_alert(sys_voice_phase_t phase, const char *reason)
{
    if (!phase_transition_valid(s_phase, phase)) {
        ESP_LOGW(TAG, "非法状态转换 %d->%d，已拒绝", s_phase, phase);
        return;
    }
    s_phase = phase;
    sys_state_apply_voice(phase, s_turn_id, reason);
}
