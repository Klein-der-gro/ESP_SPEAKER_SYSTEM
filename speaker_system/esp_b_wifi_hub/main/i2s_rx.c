#include "hub.h"

#include <math.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "dsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HUB_I2S_PORT I2S_NUM_0
#define HUB_I2S_BCLK_GPIO 18
#define HUB_I2S_WS_GPIO 19
#define HUB_I2S_DATA_GPIO 23
#define HUB_I2S_READ_TIMEOUT_MS 100
#define DSP_STALE_TIMEOUT_US 5000000LL
#define SILENCE_THRESHOLD 4
#define SILENCE_HOLD_US 2000000LL

static const char *TAG = "HUB_I2S";
static i2s_chan_handle_t s_rx_channel;
static TaskHandle_t s_i2s_task;

typedef struct {
    uint32_t avg_abs;
    uint32_t peak;
} pcm_level_t;

typedef struct {
    bool have_params;
    bool stale;
    bool muted;
    int volume;
} dsp_status_t;

static pcm_level_t measure_pcm(const int16_t *samples, size_t count)
{
    pcm_level_t level = {0};
    if (count == 0U) return level;

    uint64_t abs_sum = 0U;
    for (size_t i = 0; i < count; i++) {
        int32_t sample = samples[i];
        uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);
        abs_sum += magnitude;
        if (magnitude > level.peak) level.peak = magnitude;
    }
    level.avg_abs = (uint32_t)(abs_sum / count);
    return level;
}

static esp_err_t configure_rx(uint32_t sample_rate)
{
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(HUB_I2S_PORT, I2S_ROLE_SLAVE);
    channel_config.dma_desc_num = 8;
    channel_config.dma_frame_num = HUB_CHUNK_FRAMES;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, NULL, &s_rx_channel),
                        TAG, "I2S RX channel create failed");

    i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = HUB_I2S_BCLK_GPIO,
            .ws = HUB_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = HUB_I2S_DATA_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_channel, &std_config),
                        TAG, "I2S standard mode init failed");
    return i2s_channel_enable(s_rx_channel);
}

static dsp_status_t apply_dsp(int16_t *samples, size_t frames,
                              uint32_t sample_rate)
{
    dsp_status_t status = {0};
#if DSP_ON_HUB
    protocol_dsp_params_t params = {0};
    int64_t last_seen_us = 0;
    bool have_params = hub_get_dsp_params(&params, &last_seen_us);
    bool stale = !have_params ||
                 esp_timer_get_time() - last_seen_us > DSP_STALE_TIMEOUT_US;
    status.have_params = have_params;
    status.stale = stale;
    status.muted = params.muted != 0U;
    status.volume = params.volume;
    dsp_set_params(params.bass_db, params.treble_db, params.vocal_db,
                   params.balance, params.volume,
                   stale || params.muted != 0U, params.vocal_mode);
    int64_t start_us = esp_timer_get_time();
    dsp_process(samples, frames);
    int64_t elapsed_us = esp_timer_get_time() - start_us;
    static int64_t window_start_us;
    static uint64_t window_total_us;
    static uint64_t window_max_us;
    static uint32_t window_chunks;
    if (window_start_us == 0) window_start_us = start_us;
    if (elapsed_us > 0) {
        window_total_us += (uint64_t)elapsed_us;
        if ((uint64_t)elapsed_us > window_max_us) {
            window_max_us = (uint64_t)elapsed_us;
        }
        window_chunks++;
    }
    if (start_us - window_start_us >= 5000000LL) {
        uint64_t average_us = window_chunks > 0U
            ? window_total_us / window_chunks : 0U;
        uint64_t chunk_budget_us =
            ((uint64_t)frames * 1000000U) / sample_rate / 2U;
        ESP_LOGI(TAG, "DSP avg/max=%llu/%llu us, half-chunk budget=%llu us",
                 (unsigned long long)average_us,
                 (unsigned long long)window_max_us,
                 (unsigned long long)chunk_budget_us);
        if (average_us >= chunk_budget_us || window_max_us >= chunk_budget_us) {
            ESP_LOGE(TAG, "DSP exceeds the 50%% chunk-time budget");
        }
        window_start_us = start_us;
        window_total_us = 0U;
        window_max_us = 0U;
        window_chunks = 0U;
    }
#else
    (void)samples;
    (void)frames;
    (void)sample_rate;
#endif
    return status;
}

static void i2s_rx_task(void *arg)
{
    (void)arg;
    uint8_t raw[HUB_CHUNK_FRAMES * 2U * sizeof(int16_t)];
    uint32_t rate = hub_get_sample_rate();
    uint64_t frame_index = 0U;
    int64_t silence_start_us = 0;
    int64_t last_timeout_log_us = 0;
    int64_t level_window_start_us = 0;
    uint64_t input_avg_abs_sum = 0U;
    uint64_t output_avg_abs_sum = 0U;
    uint32_t input_peak = 0U;
    uint32_t output_peak = 0U;
    uint32_t level_chunks = 0U;
    dsp_status_t current_dsp_status = {0};
#if DSP_ON_HUB
    dsp_init_muted(rate);
#endif

    for (;;) {
        uint32_t changed_rate;
        if (hub_take_sample_rate(&changed_rate)) {
            esp_err_t rate_err = i2s_channel_disable(s_rx_channel);
            if (rate_err != ESP_OK) {
                ESP_LOGE(TAG, "I2S disable for rate change failed: %s",
                         esp_err_to_name(rate_err));
                hub_set_sample_rate(changed_rate);
                continue;
            }
            i2s_std_clk_config_t clock_config =
                I2S_STD_CLK_DEFAULT_CONFIG(changed_rate);
            rate_err = i2s_channel_reconfig_std_clock(s_rx_channel,
                                                       &clock_config);
            if (rate_err != ESP_OK) {
                ESP_LOGE(TAG, "I2S clock reconfiguration failed: %s",
                         esp_err_to_name(rate_err));
                (void)i2s_channel_enable(s_rx_channel);
                hub_set_sample_rate(changed_rate);
                continue;
            }
            rate_err = i2s_channel_enable(s_rx_channel);
            if (rate_err != ESP_OK) {
                ESP_LOGE(TAG, "I2S re-enable after rate change failed: %s",
                         esp_err_to_name(rate_err));
                hub_set_sample_rate(changed_rate);
                continue;
            }
            hub_sample_rate_applied(changed_rate);
            rate = changed_rate;
            frame_index = 0U;
            silence_start_us = 0;
            xQueueReset(g_hub_audio_queue);
#if DSP_ON_HUB
            dsp_init_muted(rate);
#endif
            ESP_LOGI(TAG, "I2S input rate changed to %lu Hz",
                     (unsigned long)rate);
        }

        size_t received = 0U;
        bool read_ok = true;
        while (received < sizeof(raw)) {
            size_t bytes_read = 0U;
            esp_err_t err = i2s_channel_read(
                s_rx_channel, raw + received, sizeof(raw) - received,
                &bytes_read, HUB_I2S_READ_TIMEOUT_MS);
            if (err != ESP_OK || bytes_read == 0U) {
                read_ok = false;
                break;
            }
            received += bytes_read;
        }
        if (!read_ok) {
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_timeout_log_us >= 2000000LL) {
                ESP_LOGW(TAG, "No I2S clock/audio from ESP A yet");
                last_timeout_log_us = now_us;
            }
            continue;
        }

        hub_audio_chunk_t chunk = {0};
        memcpy(chunk.samples, raw, sizeof(raw));
        pcm_level_t input_level =
            measure_pcm(chunk.samples, HUB_CHUNK_FRAMES * 2U);
        int64_t read_time_us = esp_timer_get_time();
        chunk.hub_time_us = read_time_us -
            (int64_t)(((uint64_t)HUB_CHUNK_FRAMES * 1000000U) / rate);
        chunk.frame_index = frame_index;
        chunk.sample_rate = rate;
        frame_index += HUB_CHUNK_FRAMES;

        bool silent = true;
        for (size_t i = 0; i < HUB_CHUNK_FRAMES * 2U; i++) {
            int32_t sample = chunk.samples[i];
            if (sample < -SILENCE_THRESHOLD || sample > SILENCE_THRESHOLD) {
                silent = false;
                break;
            }
        }
        if (silent) {
            if (silence_start_us == 0) silence_start_us = read_time_us;
            if (read_time_us - silence_start_us >= SILENCE_HOLD_US) {
                chunk.flags |= PROTOCOL_FLAG_SILENCE;
            }
        } else {
            silence_start_us = 0;
        }

        current_dsp_status = apply_dsp(chunk.samples, HUB_CHUNK_FRAMES, rate);
        pcm_level_t output_level =
            measure_pcm(chunk.samples, HUB_CHUNK_FRAMES * 2U);
        input_avg_abs_sum += input_level.avg_abs;
        output_avg_abs_sum += output_level.avg_abs;
        if (input_level.peak > input_peak) input_peak = input_level.peak;
        if (output_level.peak > output_peak) output_peak = output_level.peak;
        level_chunks++;
        if (level_window_start_us == 0) level_window_start_us = read_time_us;
        if (read_time_us - level_window_start_us >= 1000000LL) {
            ESP_LOGI(TAG, "PCM input avg_abs=%lu peak=%lu post_dsp "
                     "avg_abs=%lu peak=%lu params=%u stale=%u muted=%u "
                     "volume=%d",
                     (unsigned long)(input_avg_abs_sum / level_chunks),
                     (unsigned long)input_peak,
                     (unsigned long)(output_avg_abs_sum / level_chunks),
                     (unsigned long)output_peak,
                     current_dsp_status.have_params ? 1U : 0U,
                     current_dsp_status.stale ? 1U : 0U,
                     current_dsp_status.muted ? 1U : 0U,
                     current_dsp_status.volume);
            level_window_start_us = read_time_us;
            input_avg_abs_sum = 0U;
            output_avg_abs_sum = 0U;
            input_peak = 0U;
            output_peak = 0U;
            level_chunks = 0U;
        }
        if (xQueueSend(g_hub_audio_queue, &chunk, 0) != pdTRUE) {
            hub_note_queue_overrun();
        }
    }
}

esp_err_t hub_i2s_rx_start(void)
{
    ESP_RETURN_ON_ERROR(configure_rx(hub_get_sample_rate()), TAG,
                        "I2S RX initialization failed");
    if (xTaskCreatePinnedToCore(i2s_rx_task, "hub_i2s_rx", 6144, NULL, 8,
                                &s_i2s_task, 1) != pdPASS) {
        i2s_channel_disable(s_rx_channel);
        i2s_del_channel(s_rx_channel);
        s_rx_channel = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "I2S slave RX ready: BCLK=%d WS=%d DIN=%d, 16-bit stereo",
             HUB_I2S_BCLK_GPIO, HUB_I2S_WS_GPIO, HUB_I2S_DATA_GPIO);
    return ESP_OK;
}
