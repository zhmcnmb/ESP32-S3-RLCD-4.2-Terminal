#include "voice_assistant_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "storage_sd.h"

static const char *TAG = "voice_wav";

#define SAMPLE_RATE           16000
#define WAV_HEADER_SIZE       44
#define ASR_TRIM_TAIL_PAD_MS  400 /* 裁剪后保留的静音余量，避免切掉尾音 */

#pragma pack(push, 1)
typedef struct {
    char riff[4];
    uint32_t riff_size;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
} wav_header_t;
#pragma pack(pop)

static void build_wav_header(wav_header_t *h, uint32_t pcm_bytes)
{
    memcpy(h->riff, "RIFF", 4);
    h->riff_size = 36 + pcm_bytes;
    memcpy(h->wave, "WAVE", 4);
    memcpy(h->fmt, "fmt ", 4);
    h->fmt_size = 16;
    h->audio_format = 1;
    h->num_channels = 1;
    h->sample_rate = SAMPLE_RATE;
    h->byte_rate = SAMPLE_RATE * 2;
    h->block_align = 2;
    h->bits_per_sample = 16;
    memcpy(h->data, "data", 4);
    h->data_size = pcm_bytes;
}

size_t va_wav_trim_trailing_silence(int16_t *pcm, size_t n)
{
    int32_t peak = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > peak) peak = a;
    }
    int32_t thresh = peak / 8;
    if (thresh < 200) thresh = 200;

    size_t last_active = 0;
    for (size_t i = n; i > 0; i--) {
        int32_t a = pcm[i - 1] < 0 ? -pcm[i - 1] : pcm[i - 1];
        if (a > thresh) {
            last_active = i;
            break;
        }
    }
    if (last_active == 0) {
        return n; /* 全程无明显信号，交给下游"未听清"处理，这里不裁 */
    }
    size_t pad = SAMPLE_RATE * ASR_TRIM_TAIL_PAD_MS / 1000;
    size_t new_n = last_active + pad;
    return (new_n < n) ? new_n : n;
}

int32_t va_wav_normalize_utterance(int16_t *pcm, size_t n)
{
    int32_t peak = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > peak) peak = a;
    }
    /* 远场录音电平低，SenseVoice 对电平敏感。峰值拉到约12000，增益上限32x，
     * 峰值过低(纯静音)则跳过放大 */
    if (peak > 30 && peak < 12000) {
        int32_t gain_x16 = (12000 * 16) / peak;
        if (gain_x16 > 32 * 16) gain_x16 = 32 * 16;
        for (size_t i = 0; i < n; i++) {
            int32_t v = ((int32_t)pcm[i] * gain_x16) / 16;
            if (v > 32767) v = 32767;
            else if (v < -32768) v = -32768;
            pcm[i] = (int16_t)v;
        }
    }
    return peak;
}

esp_err_t va_wav_save_utterance(const int16_t *pcm, size_t n)
{
    if (!storage_sd_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t pcm_bytes = (uint32_t)n * 2;
    uint32_t total = WAV_HEADER_SIZE + pcm_bytes;
    uint8_t *buf = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    build_wav_header((wav_header_t *)buf, pcm_bytes);
    memcpy(buf + WAV_HEADER_SIZE, pcm, pcm_bytes);
    esp_err_t err = storage_sd_atomic_replace(WAV_REL_PATH, buf, total);
    heap_caps_free(buf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WAV写入失败: %s", esp_err_to_name(err));
    }
    return err;
}
