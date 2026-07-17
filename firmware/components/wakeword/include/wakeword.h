#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ESP-SR AFE(降噪/回声消除) + MultiNet(中文命令词"贾维斯") + VAD 封装。
 * 内部起一个task，循环从 audio_pipeline_read() 拉PCM喂给AFE，检测结果
 * 通过回调通知上层，回调在wakeword task上下文同步执行，必须简短、非阻塞
 * (不能在回调里做网络请求或阻塞SD写入)。
 *
 * 录音缓冲由本组件内部持有：SPEECH_START到SPEECH_END之间AFE已处理(降噪/
 * 回声消除后)的PCM自动累积到PSRAM缓冲，调用方通过 wakeword_get_utterance()
 * 取走。调用方不能再自行调用 audio_pipeline_read()——唯一的mic样本流被
 * 检测task持续读取，第二份reader会抢样本导致两边数据都不完整。
 *
 * MultiNet/VAD模型来自 partitions.csv 的 model 分区(spiffs)，
 * wakeword_init() 首次调用会检查分区是否已包含模型数据。
 */

typedef enum {
    WAKEWORD_EVENT_WAKE,          /* 唤醒词命中 */
    WAKEWORD_EVENT_SPEECH_START,  /* VAD: 唤醒后检测到语音开始 */
    WAKEWORD_EVENT_SPEECH_END,    /* VAD: 一句话说完(静音超过阈值) */
} wakeword_event_t;

typedef void (*wakeword_event_cb_t)(wakeword_event_t event, void *ctx);

/* 加载模型、初始化AFE实例，不启动检测任务。cb在事件发生时被调用 */
esp_err_t wakeword_init(wakeword_event_cb_t cb, void *ctx);

/* 取回SPEECH_START到SPEECH_END(或当前时刻，若仍在录音中)之间AFE已处理
 * (降噪/回声消除后)的PCM样本，最多max_count个int16样本，返回实际拷贝的
 * 样本数。只应在 WAKEWORD_EVENT_SPEECH_END 回调里或回调返回后立即调用一次
 * 取走本轮完整录音；SPEECH_START之前或WAKE事件之前调用返回0。
 *
 * 设计原因：唯一的mic样本流被wakeword内部task持续读取并喂给AFE用于
 * WakeNet/VAD推理，调用方不能再自行调用audio_pipeline_read()去"偷"同一
 * 路样本(会和检测task抢样本导致两边数据都不完整)；因此录音缓冲由wakeword
 * 内部持有，通过这个accessor交给调用方 */
size_t wakeword_get_utterance(int16_t *out, size_t max_count);

/* 启动检测task(依赖 audio_pipeline_init() 已成功) */
esp_err_t wakeword_start(void);

/* 停止检测task，可重复调用 wakeword_start() 恢复 */
esp_err_t wakeword_stop(void);

esp_err_t wakeword_deinit(void);

#ifdef __cplusplus
}
#endif
