/*
 * 音频管道实现：ES7210(录音ADC) + ES8311(播放DAC)，基于 esp_codec_dev 组件。
 * 引脚定义见 board_rlcd42.h，I2C 总线复用 board_i2c_bus()。
 *
 * 设计要点：
 *   - 采样率固定 16kHz mono 16-bit PCM（与 ASR/TTS 一致）
 *   - ES7210 只选 MIC1，其余 TDM 槽位留给未来 AEC 回采
 *   - PA_EN 由本模块用普通 GPIO 手动管理：写入期间拉高，结束/超时/cancel 后拉低
 *   - read/write 用 i2s_channel_read/write 直连 DMA，便于按 chunk 实现超时与 cancel
 *   - esp_codec_dev 仅用于 codec 控制(open/close/gain/vol)，不参与数据搬运
 *   - codec 实例构造细节拆分到 codec_dev_factory.c，本文件只负责对外 API
 */

#include "audio_pipeline.h"
#include "codec_dev_factory.h"

#include <string.h>
#include <stdatomic.h>

#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_codec_dev_defaults.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_rlcd42.h"

static const char *TAG = "audio_pipeline";

/* ─────────────────── 模块状态 ─────────────────── */

typedef struct {
    i2s_chan_handle_t       i2s_tx;
    i2s_chan_handle_t       i2s_rx;
    esp_codec_dev_handle_t  es8311_dev;
    esp_codec_dev_handle_t  es7210_dev;
    atomic_bool             cancel;
    bool                    ready;
} ap_state_t;

static ap_state_t s_state;

/* ─────────────────── I2S 初始化/释放 ─────────────────── */

static void deinit_i2s_channels(void);

/* 初始化 I2S 双工通道（TX + RX 共享一组引脚），失败返回非 ESP_OK */
static esp_err_t init_i2s_channels(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_state.i2s_tx, &s_state.i2s_rx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_PIPELINE_SAMPLE_RATE),
        /* 单声道 Philips 模式，16bit */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(16, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK_PIN,
            .bclk = BOARD_I2S_BCLK_PIN,
            .ws   = BOARD_I2S_LRCLK_PIN,
            .dout = BOARD_I2S_DOUT_PIN,
            .din  = BOARD_I2S_DIN_PIN,
        },
    };
    /* mono 模式下默认只发送/接收 slot 0（左声道） */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ret = i2s_channel_init_std_mode(s_state.i2s_tx, &std_cfg);
    if (ret == ESP_OK) ret = i2s_channel_init_std_mode(s_state.i2s_rx, &std_cfg);
    if (ret == ESP_OK) ret = i2s_channel_enable(s_state.i2s_tx);
    if (ret == ESP_OK) ret = i2s_channel_enable(s_state.i2s_rx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S通道初始化失败: %s", esp_err_to_name(ret));
        deinit_i2s_channels(); /* 半初始化的通道立即回收，不占用I2S外设 */
    }
    return ret;
}

/* 对称释放 I2S 通道 */
static void deinit_i2s_channels(void)
{
    if (s_state.i2s_rx) {
        i2s_channel_disable(s_state.i2s_rx);
        i2s_del_channel(s_state.i2s_rx);
        s_state.i2s_rx = NULL;
    }
    if (s_state.i2s_tx) {
        i2s_channel_disable(s_state.i2s_tx);
        i2s_del_channel(s_state.i2s_tx);
        s_state.i2s_tx = NULL;
    }
}

/* ─────────────────── 公开 API ─────────────────── */

esp_err_t audio_pipeline_init(void)
{
    if (s_state.ready) {
        ESP_LOGW(TAG, "audio_pipeline 已初始化");
        return ESP_OK;
    }
    memset(&s_state, 0, sizeof(s_state));
    atomic_init(&s_state.cancel, false);

    /* PA_EN 初始先拉低，避免开机爆音 */
    gpio_config_t pa_cfg = {
        .pin_bit_mask = (1ULL << BOARD_PA_EN_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&pa_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PA_EN gpio_config 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    gpio_set_level(BOARD_PA_EN_PIN, 0);

    ret = init_i2s_channels();
    if (ret != ESP_OK) return ret;

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (gpio_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_gpio 失败");
        deinit_i2s_channels();
        return ESP_FAIL;
    }

    s_state.es8311_dev = ap_create_es8311(s_state.i2s_tx, gpio_if);
    if (s_state.es8311_dev == NULL) {
        audio_codec_delete_gpio_if(gpio_if);
        deinit_i2s_channels();
        return ESP_FAIL;
    }
    s_state.es7210_dev = ap_create_es7210(s_state.i2s_rx);
    if (s_state.es7210_dev == NULL) {
        esp_codec_dev_close(s_state.es8311_dev);
        deinit_i2s_channels();
        return ESP_FAIL;
    }

    s_state.ready = true;
    ESP_LOGI(TAG, "audio_pipeline 就绪: ES8311(play) + ES7210(rec), %dHz mono 16bit",
             AUDIO_PIPELINE_SAMPLE_RATE);
    return ESP_OK;
}

bool audio_pipeline_is_ready(void)
{
    return s_state.ready;
}

/* 每次 chunk 大小：160 个 int16 样本 ≈ 10ms @16kHz */
#define AP_CHUNK_SAMPLES  160

size_t audio_pipeline_read(int16_t *buf, size_t count, uint32_t timeout_ms)
{
    if (!s_state.ready || buf == NULL || count == 0) return 0;
    if (atomic_load(&s_state.cancel)) return 0;

    const size_t   sample_bytes = sizeof(int16_t);
    const uint32_t chunk_ms     = (AP_CHUNK_SAMPLES * 1000) / AUDIO_PIPELINE_SAMPLE_RATE;
    uint32_t       elapsed_ms   = 0;
    size_t         done         = 0;

    while (done < count) {
        if (atomic_load(&s_state.cancel)) break;
        if (timeout_ms != 0 && elapsed_ms >= timeout_ms) break;

        size_t want = count - done;
        if (want > AP_CHUNK_SAMPLES) want = AP_CHUNK_SAMPLES;

        uint32_t wait_ms = (timeout_ms == 0) ? portMAX_DELAY
                         : (timeout_ms - elapsed_ms > chunk_ms) ? chunk_ms
                         : (timeout_ms - elapsed_ms);
        size_t bytes_read = 0;
        esp_err_t r = i2s_channel_read(s_state.i2s_rx,
                                       (char *)(buf + done),
                                       want * sample_bytes,
                                       &bytes_read,
                                       pdMS_TO_TICKS(wait_ms));
        if (r == ESP_ERR_TIMEOUT) break;
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_read 错误: %s", esp_err_to_name(r));
            break;
        }
        size_t got = bytes_read / sample_bytes;
        done += got;
        elapsed_ms += (got * 1000) / AUDIO_PIPELINE_SAMPLE_RATE;
    }
    return done;
}

esp_err_t audio_pipeline_write(const int16_t *buf, size_t count)
{
    if (!s_state.ready || s_state.es8311_dev == NULL) return ESP_ERR_INVALID_STATE;
    if (buf == NULL || count == 0) return ESP_OK;
    if (atomic_load(&s_state.cancel)) return ESP_ERR_NOT_FINISHED;

    /* 分块写：每块之间检查 cancel，保证半双工可被打断。PA 使能由调用方
     * 通过 audio_pipeline_set_output_enable() 在会话首尾控制，本函数不碰，
     * 否则流式播放场景下每次调用都开关PA会产生咔哒声。
     * 用 esp_codec_dev_write 而非裸 i2s_channel_write：ES8311 由 esp_codec_dev
     * 管理 DAC 数据通路，裸写会绕过 codec 导致无声/失败(xiaozhi 同款做法) */
    const size_t chunk = AP_CHUNK_SAMPLES * 4; /* 640样本≈40ms，减少调用次数 */
    size_t done = 0;
    bool cancelled = false;
    while (done < count) {
        if (atomic_load(&s_state.cancel)) { cancelled = true; break; }
        size_t want = count - done;
        if (want > chunk) want = chunk;
        int rc = esp_codec_dev_write(s_state.es8311_dev,
                                     (void *)(buf + done), (int)(want * sizeof(int16_t)));
        if (rc != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "esp_codec_dev_write 错误: %d", rc);
            return ESP_FAIL;
        }
        done += want;
    }
    return cancelled ? ESP_ERR_NOT_FINISHED : ESP_OK;
}

void audio_pipeline_set_output_enable(bool enable)
{
    gpio_set_level(BOARD_PA_EN_PIN, enable ? 1 : 0);
}

void audio_pipeline_cancel(void)
{
    atomic_store(&s_state.cancel, true);
}

void audio_pipeline_clear_cancel(void)
{
    atomic_store(&s_state.cancel, false);
}
