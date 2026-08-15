#pragma once

#include "sys_state.h"

/* 设置当前 turn id，供 voice_alert 推送给 sys_state */
void voice_phase_set_turn(uint32_t turn_id);

/* 当前相位（供回调决定是否还需要推送 UI 状态） */
sys_voice_phase_t voice_phase_get(void);

/* 复合状态更新：校验转换合法性 + 推进相位 + 推送 sys_state(含 turn_id 与可选原因)。
 * 非法转换拒绝并记录；page_jarvis 据此区分"网络故障"/"未听清"等。 */
void voice_alert(sys_voice_phase_t phase, const char *reason);
