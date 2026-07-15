#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * P5.1 本地音频链路自检：启动 audio_pipeline + wakeword，监听唤醒词事件，
 * 唤醒后录音生成临时WAV写SD，验证半双工取消机制。临时验证代码，P5.2
 * 正式语音助手接入后删除。
 */
esp_err_t p51_selftest_start(void);

#ifdef __cplusplus
}
#endif
