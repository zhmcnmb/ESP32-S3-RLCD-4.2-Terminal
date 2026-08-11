#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ESP-SR AFE(降噪/VAD) 按键录音封装：无开放式唤醒词，BOOT短按PTT是唯一入口。
 * 检测task只在对话轮期间运行：wakeword_start()拉起、wakeword_trigger()
 * 注入WAKE、一轮结束wakeword_stop()完全停止，平时无常驻监听与推理开销。
 * 事件通过回调通知上层，回调在wakeword task上下文同步执行，必须简短、非阻塞
 * (不能在回调里做网络请求或阻塞SD写入)。
 *
 * 录音缓冲由本组件内部持有：SPEECH_START到SPEECH_END之间AFE已处理(降噪后)
 * 的PCM自动累积到PSRAM缓冲，调用方通过 wakeword_get_utterance() 取走。
 * 调用方不能再自行调用 audio_pipeline_read()——唯一的mic样本流被
 * 检测task持续读取，第二份reader会抢样本导致两边数据都不完整。
 */

typedef enum {
    WAKEWORD_EVENT_WAKE,          /* PTT触发(wakeword_trigger注入) */
    WAKEWORD_EVENT_SPEECH_START,  /* VAD: 触发后检测到语音开始 */
    WAKEWORD_EVENT_SPEECH_END,    /* VAD: 一句话说完(静音超过阈值) */
} wakeword_event_t;

typedef void (*wakeword_event_cb_t)(wakeword_event_t event, void *ctx);

/* 初始化AFE实例，不启动检测任务。cb在事件发生时被调用 */
esp_err_t wakeword_init(wakeword_event_cb_t cb, void *ctx);

/* 取回SPEECH_START到SPEECH_END(或当前时刻，若仍在录音中)之间AFE已处理
 * 的PCM样本，最多max_count个int16样本，返回实际拷贝的样本数。
 * 只应在 WAKEWORD_EVENT_SPEECH_END 回调里或回调返回后立即调用一次
 * 取走本轮完整录音；SPEECH_START之前或WAKE事件之前调用返回0。
 *
 * 设计原因：唯一的mic样本流被wakeword内部task持续读取并喂给AFE用于
 * VAD判定，调用方不能再自行调用audio_pipeline_read()去"偷"同一
 * 路样本(会和检测task抢样本导致两边数据都不完整)；因此录音缓冲由wakeword
 * 内部持有，通过这个accessor交给调用方 */
size_t wakeword_get_utterance(int16_t *out, size_t max_count);

/* 启动录音task(依赖 audio_pipeline_init() 已成功)；PTT对话轮开始时调用 */
esp_err_t wakeword_start(void);

/* 停止录音task；对话轮结束(成功/失败/取消)都必须调用，回到无常驻监听状态 */
esp_err_t wakeword_stop(void);

/* 手动触发一轮录音(push-to-talk)：注入WAKE事件并进入LISTENING，由VAD
 * 判定语音段。仅 wakeword_start() 之后、STATE_IDLE 时有效；与对话轮
 * 并发时由 voice_assistant 的 s_in_turn 守卫保证只有一轮生效。 */
esp_err_t wakeword_trigger(void);

#ifdef __cplusplus
}
#endif
