#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 单轮语音编排：Wake -> ASR -> Chat -> TTS。
 * 对外只暴露 start；turn_id、取消、相位推进和资源释放都收在实现里。
 * 状态经 sys_state_apply_voice() 发布，页面只读快照。
 *
 * 前置: audio_pipeline_init / asr_client_init / tts_client_init /
 *       llm_provider_init 已完成(失败则本组件启动后保持降级空闲)。
 *
 * 任务: voice_assistant_task, 栈12288, 优先级4
 *       (登记于 docs/coding-standards.md 第7节)
 */
esp_err_t voice_assistant_start(void);

#ifdef __cplusplus
}
#endif
