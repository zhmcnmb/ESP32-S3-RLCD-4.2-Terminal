#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wakeword.h"
#include "audio_pipeline.h"

#define WAKEWORD_UTTERANCE_MAX_SECONDS 15
#define WAKEWORD_UTTERANCE_MAX_SAMPLES \
    (WAKEWORD_UTTERANCE_MAX_SECONDS * AUDIO_PIPELINE_SAMPLE_RATE)

typedef enum {
    STATE_IDLE,
    STATE_LISTENING,
} detect_state_t;

typedef struct {
    wakeword_event_cb_t cb;
    void *ctx;
    const esp_afe_sr_iface_t *afe_handle;
    esp_afe_sr_data_t *afe_data;
    srmodel_list_t *models;
    TaskHandle_t volatile task; /* 检测任务退出前自清，wakeword_stop以此join */
    volatile bool running;
    volatile detect_state_t state;
    bool vad_was_active;
    int16_t *utterance_buf;
    size_t utterance_count;
    size_t utterance_cap;
} wakeword_context_t;

extern wakeword_context_t g_wakeword_ctx;

void wakeword_task(void *arg);
