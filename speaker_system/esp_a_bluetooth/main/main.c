/*
 * ESP32 Bluetooth-Lautsprecher (A2DP Sink) mit I2S-DAC (z.B. PCM5102)
 *
 * Verdrahtung:
 *   GPIO 18 -> BCK  (Bit Clock)
 *   GPIO 19 -> LCK  (Word Select / LRCK), nicht an GND anschliessen
 *   GPIO 23 -> DIN  (Daten)
 *   GND     -> GND des DACs (an einen echten GND-Pin des ESP32)
 *
 *   GPIO 33 -> Encoder 1 CLK (A)
 *   GPIO 27 -> Encoder 1 DT  (B)
 *   GPIO 14 -> Encoder 1 SW (Taster: 1x Play/Pause, 2x naechster Titel,
 *                            3x vorheriger Titel)
 *   GPIO 32 -> Encoder 2 CLK (A), GPIO 25 -> Encoder 2 DT (B)
 *   GPIO 26 -> Encoder 2 SW (Taster: seitenabhaengige Aktionen, langer Druck
 *                            Vocal-Modus bzw. Stummschaltung)
 *   GPIO 21 -> OLED SDA, GPIO 22 -> OLED SCL, OLED VCC -> 3.3 V
 *   Encoder common/GND -> GND.  Taster und Signale nutzen interne Pull-ups.
 *
 *   Bedienung:
 *   - Encoder 1 drehen: Lautstaerke auf jeder OLED-Seite einstellen.
 *   - Encoder 1 druecken: 1x Play/Pause, 2x naechster, 3x vorheriger Titel.
 *   - Encoder 2 drehen: OLED-Seiten vor/zurueck wechseln.
 *   - Encoder 2 druecken: bisherige seitenabhaengige Klick-/Langdruckaktionen.
 *
 * Firmware map:
 *   - Bluetooth A2DP sends stereo PCM to s_stream; i2s_task applies DSP and
 *     volume, then writes the result to the I2S DAC.
 *   - AVRCP supplies track metadata/status and receives encoder playback keys.
 *   - ui_task renders eight OLED pages; encoder_task handles controls.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "dsp.h"
#include "link_tx.h"
#include "protocol.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"

#include "oled.h"

/* ---------- Einstellungen ---------- */
#define BT_DEVICE_NAME   "ESP32-Speaker"

#define I2S_BCK_PIN      18
#define I2S_LCK_PIN      19
#define I2S_DIN_PIN      23

#define ENC_CLK_PIN      33
#define ENC_DT_PIN       27
#define ENC_SW_PIN       14
#define PAGE_ENC_CLK_PIN 32
#define PAGE_ENC_DT_PIN  25
#define PAGE_ENC_SW_PIN  26
#define ENC_DEBUG        0

#define VOLUME_MAX       DSP_VOLUME_MAX
#define VOLUME_START     50   /* 0..VOLUME_MAX */
#define VOLUME_STEP      2    /* pro Raststellung des Encoders */
#define BASS_BOOST_STEP  10
#define BASS_BOOST_MIN   -100
#define BASS_BOOST_MAX   400
#define TREBLE_BOOST_STEP 10
#define TREBLE_BOOST_MIN  -100
#define TREBLE_BOOST_MAX  450
#define VOCAL_BOOST_STEP  10
#define VOCAL_BOOST_MIN   -100
#define VOCAL_BOOST_MAX   200
#define SOUND_PRESET_COUNT 4
#define BALANCE_STEP      10
#define BALANCE_MIN       -100
#define BALANCE_MAX       100
#define VIZ_SENSITIVITY_MIN 50
#define VIZ_SENSITIVITY_MAX 250
#define VIZ_SENSITIVITY_STEP 25
#define WAVEFORM_SAMPLES 64
#define VIZ_LEVEL_SCALE 10000U
#define EQ_PI 3.14159265358979323846f
#define VOCAL_LOW_CUTOFF_HZ 130.0f
#define VOCAL_HIGH_CUTOFF_HZ 4100.0f
#define EQ_DB_PER_CONTROL_UNIT 0.10f
#define BASS_EQ_DB_PER_CONTROL_UNIT 0.15f
#define VOCAL_MODE_OFF 0
#define VOCAL_MODE_ISOLATE 1
#define VOCAL_MODE_REMOVE 2
#define LONG_PRESS_MS    800
#define OLED_IDLE_TIMEOUT_MS 60000
#define RECONNECT_RETRY_DELAY_MS 30000

#define STREAM_BUF_SIZE  (32 * 1024)   /* muss durch 4 teilbar sein */
#define AUDIO_PREBUFFER_BYTES (4 * 1024)
#define I2S_CHUNK_BYTES  1024
#define DOUBLE_CLICK_MS  400
#define SEEK_HOLD_MS     500
#define SEEK_MIN_INTERVAL_MS 150
#define SEEK_STATUS_DELAY_MS 300
#define SEEK_MARKER_MS   500
#define BUTTON_EDGE_GUARD_MS 60
#define META_TEXT_SIZE   64
#define OLED_PAGE_COUNT  8

static const char *TAG = "BT_DAC";
static portMUX_TYPE s_bt_flags_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_audio_stats_mux = portMUX_INITIALIZER_UNLOCKED;

static StreamBufferHandle_t s_stream;
static i2s_chan_handle_t s_tx_chan;
static volatile uint32_t s_pending_rate = 0;   /* neue Samplerate, wird im I2S-Task gesetzt */
static volatile uint32_t s_sample_rate = 44100;
static bool s_link_tx_ready;
static bool s_link_tx_error_logged;
static volatile int      s_volume = VOLUME_START;
static volatile int      s_bass_boost;
static volatile int      s_treble_boost;
static volatile int      s_vocal_boost;
static volatile int      s_balance;
static volatile uint8_t  s_oled_page = 0;
static volatile bool s_page_forward = true;
static volatile bool     s_page_button_pressed;
static uint32_t s_audio_overrun_count;
static uint32_t s_audio_underrun_count;
static volatile int8_t   s_seek_marker_direction;
static volatile TickType_t s_seek_marker_until;
static volatile uint8_t  s_preset_index;
static volatile uint8_t  s_settings_selection;
static volatile bool     s_startup_chime = true;
static volatile bool     s_visualizer_fast;
static volatile bool     s_wave_mode = true;
static volatile int      s_visualizer_sensitivity = 100;
static volatile bool     s_vocal_boost_selected;
static volatile int      s_vocal_mode;
static volatile bool     s_muted;
static volatile TickType_t s_audio_active_until;
static volatile TickType_t s_last_input_tick;
static volatile bool     s_bt_ready;
static volatile bool     s_last_device_valid;
static volatile bool     s_reconnect_in_progress;
static volatile TickType_t s_reconnect_after;
static esp_bd_addr_t s_last_device;
static volatile bool     s_last_device_save_pending;
static volatile uint8_t  s_spectrum_levels[8];
static volatile uint8_t  s_vu_levels[2];
static volatile int8_t   s_waveform[2][WAVEFORM_SAMPLES];
static volatile int8_t   s_vocal_waveform[WAVEFORM_SAMPLES];
static volatile TickType_t s_clip_until;
static volatile bool     s_avrc_connected = false;
static volatile bool     s_a2dp_connected = false;
static volatile bool     s_playing = false;
static volatile bool     s_req_notification = false;
static volatile bool     s_req_metadata = false;
static volatile bool     s_clear_metadata = false;
static uint32_t s_track_length_ms;
static uint32_t s_track_position_ms;
static TickType_t s_status_updated_tick;

static SemaphoreHandle_t s_meta_mutex;
static char s_title[META_TEXT_SIZE];
static char s_artist[META_TEXT_SIZE];
static uint32_t s_meta_version;
static portMUX_TYPE s_encoder_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_encoder_state;
static int s_encoder_delta;
static uint8_t s_encoder_edge_mask;
static portMUX_TYPE s_page_encoder_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_page_encoder_state;
static int s_page_encoder_delta;
static portMUX_TYPE s_visualizer_mux = portMUX_INITIALIZER_UNLOCKED;

static const int8_t s_encoder_table[16] = {
    0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0
};

/* Page 4 offers these four fixed combinations of bass/treble processing. */
static const char *s_preset_names[SOUND_PRESET_COUNT] = {
    "FLAT", "BASS", "TREBLE", "VOCAL"
};

static const int8_t s_wave_shape[32] = {
     0,  2,  4,  6,  7,  9,  9, 10, 10, 10,  9,  9,  7,  6,  4,  2,
     0, -2, -4, -6, -7, -9, -9,-10,-10,-10, -9, -9, -7, -6, -4, -2
};

/* Bresenham line drawing keeps waveform traces smooth on the 1-bit OLED. */
static void ui_draw_line(int x0, int y0, int x1, int y1)
{
    int dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 >= y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    while (true) {
        oled_fill_rect(x0, y0, 1, 1, true);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int doubled_error = 2 * error;
        if (doubled_error >= dy) {
            error += dy;
            x0 += sx;
        }
        if (doubled_error <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static void encoder_gpio_isr(void *arg)
{
    /* Decode quadrature edges here; encoder_task consumes the accumulated steps. */
    uint8_t current = (uint8_t)((gpio_get_level(ENC_CLK_PIN) << 1) |
                                 gpio_get_level(ENC_DT_PIN));
    int pin = (int)(intptr_t)arg;
    portENTER_CRITICAL_ISR(&s_encoder_mux);
    s_encoder_delta += s_encoder_table[(s_encoder_state << 2) | current];
    s_encoder_state = current;
    s_encoder_edge_mask |= pin == ENC_CLK_PIN ? 1 : 2;
    portEXIT_CRITICAL_ISR(&s_encoder_mux);
}

static void page_encoder_gpio_isr(void *arg)
{
    (void)arg;
    uint8_t current = (uint8_t)((gpio_get_level(PAGE_ENC_CLK_PIN) << 1) |
                                 gpio_get_level(PAGE_ENC_DT_PIN));
    portENTER_CRITICAL_ISR(&s_page_encoder_mux);
    s_page_encoder_delta +=
        s_encoder_table[(s_page_encoder_state << 2) | current];
    s_page_encoder_state = current;
    portEXIT_CRITICAL_ISR(&s_page_encoder_mux);
}

static esp_err_t save_audio_settings(int volume, int bass, int treble, int vocal, int balance,
                                     bool chime, bool visualizer_fast,
                                     int visualizer_sensitivity,
                                     int vocal_mode)
{
    /* Persist user settings together so a single NVS commit saves a consistent set. */
    nvs_handle_t handle;
    esp_err_t err = nvs_open("speaker", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_i32(handle, "volume", volume);
    if (err == ESP_OK) err = nvs_set_i32(handle, "bass", bass);
    if (err == ESP_OK) err = nvs_set_i32(handle, "treble", treble);
    if (err == ESP_OK) err = nvs_set_i32(handle, "vocal", vocal);
    if (err == ESP_OK) err = nvs_set_i32(handle, "balance", balance);
    if (err == ESP_OK) err = nvs_set_i32(handle, "chime", chime ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_i32(handle, "viz_fast", visualizer_fast ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_i32(handle, "viz_sens", visualizer_sensitivity);
    if (err == ESP_OK) err = nvs_set_i32(handle, "vocal_mode", vocal_mode);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static void load_audio_settings(void)
{
    /* Load only values in their supported ranges; absent keys retain defaults. */
    nvs_handle_t handle;
    esp_err_t err = nvs_open("speaker", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Keine gespeicherten Audio-Einstellungen; Standardwerte werden verwendet");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Audio-Einstellungen konnten nicht geoeffnet werden: %s",
                 esp_err_to_name(err));
        return;
    }

    int32_t value;
    err = nvs_get_i32(handle, "volume", &value);
    if (err == ESP_OK) {
        if (value >= 0 && value <= VOLUME_MAX) s_volume = value;
        else ESP_LOGW(TAG, "Gespeicherte Lautstaerke ausserhalb des Bereichs");
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Lautstaerke konnte nicht geladen werden: %s", esp_err_to_name(err));
    }

    err = nvs_get_i32(handle, "bass", &value);
    if (err == ESP_OK) {
        if (value >= BASS_BOOST_MIN && value <= BASS_BOOST_MAX) s_bass_boost = value;
        else ESP_LOGW(TAG, "Gespeicherter Basswert ausserhalb des Bereichs");
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Basswert konnte nicht geladen werden: %s", esp_err_to_name(err));
    }

    err = nvs_get_i32(handle, "treble", &value);
    if (err == ESP_OK) {
        if (value >= TREBLE_BOOST_MIN && value <= TREBLE_BOOST_MAX) s_treble_boost = value;
        else ESP_LOGW(TAG, "Gespeicherter Hoehenwert ausserhalb des Bereichs");
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Hoehenwert konnte nicht geladen werden: %s", esp_err_to_name(err));
    }

    err = nvs_get_i32(handle, "vocal", &value);
    if (err == ESP_OK) {
        if (value >= VOCAL_BOOST_MIN && value <= VOCAL_BOOST_MAX) {
            s_vocal_boost = value;
        } else {
            ESP_LOGW(TAG, "Gespeicherter Vocal-Boost ausserhalb des Bereichs");
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Vocal-Boost konnte nicht geladen werden: %s", esp_err_to_name(err));
    }

    err = nvs_get_i32(handle, "balance", &value);
    if (err == ESP_OK) {
        if (value >= BALANCE_MIN && value <= BALANCE_MAX) s_balance = value;
        else ESP_LOGW(TAG, "Gespeicherter Balancewert ausserhalb des Bereichs");
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Balance konnte nicht geladen werden: %s", esp_err_to_name(err));
    }

    err = nvs_get_i32(handle, "chime", &value);
    if (err == ESP_OK) s_startup_chime = value != 0;
    else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Startton-Einstellung konnte nicht geladen werden: %s",
                 esp_err_to_name(err));
    }

    err = nvs_get_i32(handle, "viz_fast", &value);
    if (err == ESP_OK) s_visualizer_fast = value != 0;
    else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Visualizer-Einstellung konnte nicht geladen werden: %s",
                 esp_err_to_name(err));
    }

    err = nvs_get_i32(handle, "viz_sens", &value);
    if (err == ESP_OK) {
        if (value >= VIZ_SENSITIVITY_MIN && value <= VIZ_SENSITIVITY_MAX) {
            s_visualizer_sensitivity = value;
        } else {
            ESP_LOGW(TAG, "Gespeicherte Visualizer-Empfindlichkeit ausserhalb des Bereichs");
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Visualizer-Empfindlichkeit konnte nicht geladen werden: %s",
                 esp_err_to_name(err));
    }

    err = nvs_get_i32(handle, "vocal_mode", &value);
    if (err == ESP_OK) {
        if (value >= VOCAL_MODE_OFF && value <= VOCAL_MODE_REMOVE) {
            s_vocal_mode = value;
        } else {
            ESP_LOGW(TAG, "Gespeicherter Vocal-Modus ausserhalb des Bereichs");
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_get_i32(handle, "vocal_iso", &value);
        if (err == ESP_OK) {
            s_vocal_mode = value != 0 ? VOCAL_MODE_ISOLATE : VOCAL_MODE_OFF;
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "Vocal-Isolation konnte nicht geladen werden: %s",
                     esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Vocal-Modus konnte nicht geladen werden: %s",
                 esp_err_to_name(err));
    }

    size_t address_size = sizeof(s_last_device);
    err = nvs_get_blob(handle, "last_device", s_last_device, &address_size);
    if (err == ESP_OK && address_size == sizeof(s_last_device)) {
        s_last_device_valid = true;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Letztes Bluetooth-Geraet konnte nicht geladen werden: %s",
                 esp_err_to_name(err));
    }
    nvs_close(handle);
}

static esp_err_t save_last_device(void)
{
    /* Store the address from the latest A2DP connection for automatic reconnect. */
    nvs_handle_t handle;
    esp_err_t err = nvs_open("speaker", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, "last_device", s_last_device, sizeof(s_last_device));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static const int s_preset_bass[SOUND_PRESET_COUNT] = {0, 100, 0, -20};
static const int s_preset_treble[SOUND_PRESET_COUNT] = {0, 0, 100, 40};

static void format_eq_gain(char *text, size_t text_size, int control_value,
                           float db_per_control_unit)
{
    float tenths_db_float = control_value * db_per_control_unit * 10.0f;
    int tenths_db = (int)(tenths_db_float +
                          (tenths_db_float >= 0.0f ? 0.5f : -0.5f));
    unsigned magnitude = (unsigned)(tenths_db < 0 ? -tenths_db : tenths_db);
    snprintf(text, text_size, "%c%u.%u",
             tenths_db < 0 ? '-' : '+', magnitude / 10U, magnitude % 10U);
}

static void apply_sound_preset(uint8_t preset)
{
    if (preset >= SOUND_PRESET_COUNT) {
        ESP_LOGE(TAG, "Ungueltiges Klang-Preset: %u", (unsigned)preset);
        return;
    }
    s_preset_index = preset;
    s_bass_boost = s_preset_bass[preset];
    s_treble_boost = s_preset_treble[preset];
    ESP_LOGI(TAG, "Klang-Preset aktiviert: %s", s_preset_names[preset]);
}

static int32_t vocal_filter_coefficient(float cutoff_hz, uint32_t sample_rate)
{
    if (sample_rate == 0) {
        return 0;
    }
    float coefficient = 1.0f - expf(-2.0f * EQ_PI * cutoff_hz /
                                    (float)sample_rate);
    if (coefficient > 0.999f) {
        coefficient = 0.999f;
    }
    return (int32_t)(coefficient * 32768.0f);
}

static void play_startup_tone(uint32_t frequency, uint32_t duration_ms)
{
    /* Generate a short stereo sine tone directly into the I2S output buffer. */
    int16_t samples[512];
    uint32_t rate = s_sample_rate;
    uint32_t phase = 0;
    uint32_t phase_step = (frequency * 65536U) / rate;
    uint32_t total_frames = (rate * duration_ms) / 1000U;
    uint32_t frames_written = 0;

    while (frames_written < total_frames) {
        size_t frames = total_frames - frames_written;
        if (frames > (sizeof(samples) / (2 * sizeof(samples[0])))) {
            frames = sizeof(samples) / (2 * sizeof(samples[0]));
        }
        for (size_t frame = 0; frame < frames; frame++) {
            uint32_t position = phase & 0xFFFFU;
            int32_t amplitude = 1800;
            if (frames_written + frame > total_frames - (rate / 40U)) {
                uint32_t remaining = total_frames - (frames_written + frame);
                amplitude = (int32_t)((1800U * remaining) / (rate / 40U));
            }
            int32_t sample = position < 32768U
                ? -amplitude + (int32_t)((position * 2U * (uint32_t)amplitude) / 32768U)
                : amplitude - (int32_t)(((position - 32768U) * 2U *
                                          (uint32_t)amplitude) / 32768U);
            samples[frame * 2] = (int16_t)sample;
            samples[frame * 2 + 1] = (int16_t)sample;
            phase += phase_step;
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, samples,
                                          frames * 2 * sizeof(samples[0]),
                                          &written, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Startton konnte nicht ausgegeben werden: %s",
                     esp_err_to_name(err));
            return;
        }
        frames_written += (uint32_t)frames;
    }
}

static void bt_schedule_requests(bool notification, bool metadata, bool clear_metadata)
{
    /* Bluetooth callbacks only set flags; ui_task performs AVRCP requests outside callbacks. */
    portENTER_CRITICAL(&s_bt_flags_mux);
    s_req_notification |= notification;
    s_req_metadata |= metadata;
    s_clear_metadata |= clear_metadata;
    portEXIT_CRITICAL(&s_bt_flags_mux);
}

static void bt_take_requests(bool *notification, bool *metadata, bool *clear_metadata)
{
    portENTER_CRITICAL(&s_bt_flags_mux);
    *notification = s_req_notification;
    *metadata = s_req_metadata;
    *clear_metadata = s_clear_metadata;
    s_req_notification = false;
    s_req_metadata = false;
    s_clear_metadata = false;
    portEXIT_CRITICAL(&s_bt_flags_mux);
}

/* ---------- I2S ---------- */
static TaskHandle_t s_i2s_task_handle;
static TaskHandle_t s_encoder_task_handle;
static TaskHandle_t s_ui_task_handle;

static void i2s_init(void)
{
    /* Configure the ESP32 as the I2S clock master and transmit stereo 16-bit PCM. */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;   /* bei Pufferleerlauf Stille statt Knacken */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_PIN,
            .ws   = I2S_LCK_PIN,
            .dout = I2S_DIN_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
}

static protocol_dsp_params_t current_dsp_params(void)
{
    return (protocol_dsp_params_t) {
        .bass_db = s_bass_boost * BASS_EQ_DB_PER_CONTROL_UNIT,
        .treble_db = s_treble_boost * EQ_DB_PER_CONTROL_UNIT,
        .vocal_db = s_vocal_boost * EQ_DB_PER_CONTROL_UNIT,
        .balance = (int16_t)s_balance,
        .volume = (int16_t)s_volume,
        .muted = s_muted ? 1U : 0U,
        .vocal_mode = (uint8_t)s_vocal_mode,
    };
}

static void send_dsp_params_to_hub(void)
{
#if DSP_ON_HUB
    if (!s_link_tx_ready) return;
    protocol_dsp_params_t params = current_dsp_params();
    esp_err_t err = link_tx_send_dsp_params(&params);
    if (err != ESP_OK && !s_link_tx_error_logged) {
        ESP_LOGE(TAG, "DSP parameters could not be sent to ESP B: %s",
                 esp_err_to_name(err));
        s_link_tx_error_logged = true;
    }
#endif
}

/* Liest PCM-Daten aus dem Stream-Puffer, wendet die Lautstaerke an und gibt sie per I2S aus */
static void i2s_task(void *arg)
{
    static uint8_t buf[I2S_CHUNK_BYTES];
    static int16_t vocal_band_chunk[I2S_CHUNK_BYTES / 4];
    int32_t vocal_mono_low = 0;
    int32_t vocal_mono_high = 0;
    int32_t vocal_low_coefficient =
        vocal_filter_coefficient(VOCAL_LOW_CUTOFF_HZ, s_sample_rate);
    int32_t vocal_high_coefficient =
        vocal_filter_coefficient(VOCAL_HIGH_CUTOFF_HZ, s_sample_rate);
    int32_t spectrum_filters[7] = {0};
    static const int32_t spectrum_coefficients[7] = {
        700, 1400, 2800, 5400, 10000, 16000, 24500
    };
    int64_t processing_window_start = esp_timer_get_time();
    uint64_t processing_window_total_us = 0;
    uint64_t processing_window_max_us = 0;
    uint32_t processing_window_chunks = 0;
    bool playback_started = false;

    dsp_init(s_sample_rate);

    if (s_startup_chime) {
        play_startup_tone(523, 110);
        play_startup_tone(659, 130);
    }

    while (1) {
        if (s_pending_rate) {
            uint32_t rate = s_pending_rate;
            s_pending_rate = 0;
            i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
            i2s_channel_disable(s_tx_chan);
            i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg);
            i2s_channel_enable(s_tx_chan);
            s_sample_rate = rate;
            esp_err_t link_err = s_link_tx_ready
                ? link_tx_send_rate(rate) : ESP_ERR_INVALID_STATE;
            if (s_link_tx_ready && link_err != ESP_OK) {
                ESP_LOGE(TAG, "Sample rate could not be sent to ESP B: %s",
                         esp_err_to_name(link_err));
            }
            vocal_low_coefficient =
                vocal_filter_coefficient(VOCAL_LOW_CUTOFF_HZ, rate);
            vocal_high_coefficient =
                vocal_filter_coefficient(VOCAL_HIGH_CUTOFF_HZ, rate);
            dsp_init(rate);
            ESP_LOGI(TAG, "Samplerate: %lu Hz", (unsigned long)rate);
        }

        size_t buffered_bytes = xStreamBufferBytesAvailable(s_stream);
        if (!playback_started && buffered_bytes < AUDIO_PREBUFFER_BYTES) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        playback_started = true;

        size_t received = xStreamBufferReceive(s_stream, buf, sizeof(buf), 0);
        if (received == 0) {
            portENTER_CRITICAL(&s_audio_stats_mux);
            s_audio_underrun_count++;
            portEXIT_CRITICAL(&s_audio_stats_mux);
            playback_started = false;
            portENTER_CRITICAL(&s_visualizer_mux);
            for (size_t band = 0; band < 8; band++) {
                s_spectrum_levels[band] = (uint8_t)((s_spectrum_levels[band] * 7U) / 8U);
            }
            for (size_t channel = 0; channel < 2; channel++) {
                s_vu_levels[channel] =
                    (uint8_t)((s_vu_levels[channel] * 7U) / 8U);
            }
            for (size_t channel = 0; channel < 2; channel++) {
                for (size_t sample = 0; sample < WAVEFORM_SAMPLES; sample++) {
                    s_waveform[channel][sample] =
                        (int8_t)((s_waveform[channel][sample] * 7) / 8);
                }
            }
            for (size_t sample = 0; sample < WAVEFORM_SAMPLES; sample++) {
                s_vocal_waveform[sample] =
                    (int8_t)((s_vocal_waveform[sample] * 7) / 8);
            }
            portEXIT_CRITICAL(&s_visualizer_mux);
            continue;
        }

        size_t n = received & ~3u;   /* nur ganze Stereo-Frames (4 Byte) */
        size_t frames = n / 4;
        int64_t processing_start = esp_timer_get_time();
        uint64_t band_sums[8] = {0};
        uint64_t signal_sum = 0;
        bool chunk_clipped = false;
        int16_t *s = (int16_t *)buf;
        for (size_t frame = 0; frame < frames; frame++) {
            int32_t left = s[frame * 2];
            int32_t right = s[frame * 2 + 1];
            int32_t mono = (left + right) / 2;
            signal_sum += (uint32_t)(left < 0 ? -left : left);
            signal_sum += (uint32_t)(right < 0 ? -right : right);
            int32_t previous_lowpass = 0;
            for (size_t band = 0; band < 7; band++) {
                int32_t lowpass = spectrum_filters[band];
                lowpass += ((mono - lowpass) * spectrum_coefficients[band]) >> 15;
                spectrum_filters[band] = lowpass;
                int32_t band_sample = lowpass - previous_lowpass;
                band_sums[band] += (uint32_t)(band_sample < 0 ? -band_sample : band_sample);
                previous_lowpass = lowpass;
            }
            int32_t high_sample = mono - previous_lowpass;
            band_sums[7] += (uint32_t)(high_sample < 0 ? -high_sample : high_sample);

            vocal_mono_low += ((mono - vocal_mono_low) * vocal_low_coefficient) >> 15;
            vocal_mono_high += ((mono - vocal_mono_high) * vocal_high_coefficient) >> 15;
            int32_t vocal_mono = vocal_mono_high - vocal_mono_low;
            vocal_band_chunk[frame] = (int16_t)vocal_mono;
        }
        if (frames > 0) {
            uint32_t signal_average = (uint32_t)(signal_sum / (frames * 2U));
            if (signal_average > 120U) {
                s_audio_active_until = xTaskGetTickCount() + pdMS_TO_TICKS(500);
            }
            int sensitivity = s_visualizer_sensitivity;
            portENTER_CRITICAL(&s_visualizer_mux);
            for (size_t band = 0; band < 8; band++) {
                /* Cascaded low-pass differences estimate eight broad spectrum bands. */
                uint32_t average = (uint32_t)(band_sums[band] / frames);
                uint32_t target = (average * (uint32_t)sensitivity) /
                                  VIZ_LEVEL_SCALE;
                if (target > 100U) target = 100U;
                uint32_t current = s_spectrum_levels[band];
                if (s_visualizer_fast) {
                    s_spectrum_levels[band] = (uint8_t)target;
                } else {
                    s_spectrum_levels[band] = (uint8_t)(target > current
                        ? current + ((target - current) * 3U) / 4U
                        : (current * 2U + target) / 3U);
                }
            }
            portEXIT_CRITICAL(&s_visualizer_mux);
        }

#if !DSP_ON_HUB
        protocol_dsp_params_t dsp_params = current_dsp_params();
        dsp_set_params(dsp_params.bass_db, dsp_params.treble_db,
                       dsp_params.vocal_db, dsp_params.balance,
                       dsp_params.volume, dsp_params.muted != 0U,
                       dsp_params.vocal_mode);
        dsp_process(s, frames);
#endif

        uint64_t vu_sums[2] = {0};
        for (size_t frame = 0; frame < frames; frame++) {
            int32_t left = s[frame * 2];
            int32_t right = s[frame * 2 + 1];
            int32_t left_magnitude = left < 0 ? -left : left;
            int32_t right_magnitude = right < 0 ? -right : right;
            if (left_magnitude >= 26000 || right_magnitude >= 26000) {
                chunk_clipped = true;
            }
            vu_sums[0] += (uint32_t)left_magnitude;
            vu_sums[1] += (uint32_t)right_magnitude;
        }
        if (frames > 0) {
            portENTER_CRITICAL(&s_visualizer_mux);
            for (size_t channel = 0; channel < 2; channel++) {
                uint32_t average = (uint32_t)(vu_sums[channel] / frames);
                uint32_t target = (average * 100U) / 12000U;
                if (target > 100U) target = 100U;
                uint32_t current = s_vu_levels[channel];
                s_vu_levels[channel] = (uint8_t)(target > current
                    ? (current + target) / 2U
                    : (current * 3U + target) / 4U);
            }
            for (size_t sample = 0; sample < WAVEFORM_SAMPLES; sample++) {
                size_t frame = sample * frames / WAVEFORM_SAMPLES;
                for (size_t channel = 0; channel < 2; channel++) {
                    s_waveform[channel][sample] =
                        (int8_t)(s[frame * 2 + channel] / 256);
                }
                s_vocal_waveform[sample] =
                    (int8_t)(vocal_band_chunk[frame] / 256);
            }
            portEXIT_CRITICAL(&s_visualizer_mux);
        }
        if (chunk_clipped) {
            s_clip_until = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
        }

        int64_t processing_elapsed_us = esp_timer_get_time() - processing_start;
        if (processing_elapsed_us > 0) {
            processing_window_total_us += (uint64_t)processing_elapsed_us;
            if ((uint64_t)processing_elapsed_us > processing_window_max_us) {
                processing_window_max_us = (uint64_t)processing_elapsed_us;
            }
            processing_window_chunks++;
        }
        int64_t log_now_us = esp_timer_get_time();
        if (log_now_us - processing_window_start >= 5000000) {
            uint64_t average_us = processing_window_chunks > 0
                ? processing_window_total_us / processing_window_chunks : 0U;
            uint64_t chunk_budget_us = frames > 0
                ? ((uint64_t)frames * 1000000U) / s_sample_rate / 2U : 0U;
            uint32_t overruns;
            uint32_t underruns;
            portENTER_CRITICAL(&s_audio_stats_mux);
            overruns = s_audio_overrun_count;
            underruns = s_audio_underrun_count;
            s_audio_overrun_count = 0;
            s_audio_underrun_count = 0;
            portEXIT_CRITICAL(&s_audio_stats_mux);
            UBaseType_t i2s_stack_free = uxTaskGetStackHighWaterMark(NULL);
            UBaseType_t encoder_stack_free = s_encoder_task_handle
                ? uxTaskGetStackHighWaterMark(s_encoder_task_handle) : 0U;
            UBaseType_t ui_stack_free = s_ui_task_handle
                ? uxTaskGetStackHighWaterMark(s_ui_task_handle) : 0U;
            ESP_LOGI(TAG, "DSP chunk avg/max=%llu/%llu us, budget=%llu us; "
                     "stream overruns/underruns=%" PRIu32 "/%" PRIu32
                     "; stack free i2s/enc/ui=%u/%u/%u words",
                     (unsigned long long)average_us,
                     (unsigned long long)processing_window_max_us,
                     (unsigned long long)chunk_budget_us,
                     overruns, underruns,
                     (unsigned)i2s_stack_free, (unsigned)encoder_stack_free,
                     (unsigned)ui_stack_free);
            if (chunk_budget_us > 0U && average_us >= chunk_budget_us) {
                ESP_LOGW(TAG, "DSP average exceeds half of the PCM chunk duration");
            }
            if (chunk_budget_us > 0U &&
                processing_window_max_us >= chunk_budget_us) {
                ESP_LOGW(TAG, "DSP maximum exceeds half of the PCM chunk duration");
            }
            if (i2s_stack_free < 512U || encoder_stack_free < 512U ||
                ui_stack_free < 512U) {
                ESP_LOGW(TAG, "A task has less than 512 words of stack headroom");
            }
            processing_window_start = log_now_us;
            processing_window_total_us = 0U;
            processing_window_max_us = 0U;
            processing_window_chunks = 0U;
        }

        size_t written = 0;
        esp_err_t write_err = i2s_channel_write(s_tx_chan, buf, n, &written,
                                                portMAX_DELAY);
        if (write_err != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(write_err));
        }
    }
}

/* ---------- Bluetooth ---------- */
static void bt_request_metadata(void)
{
    /* Request the track title and artist for the playback page. */
    esp_err_t err = esp_avrc_ct_send_metadata_cmd(
        9, ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Metadatenabfrage fehlgeschlagen: %s", esp_err_to_name(err));
    }
}

static void bt_register_track_notification(void)
{
    /* Ask the player to notify us when the active track changes. */
    esp_err_t err = esp_avrc_ct_send_register_notification_cmd(
        8, ESP_AVRC_RN_TRACK_CHANGE, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Titelbenachrichtigung fehlgeschlagen: %s", esp_err_to_name(err));
    }
}

static void bt_request_play_status(void)
{
    /* Track position and length arrive through AVRCP and drive the progress display. */
    static bool request_error_logged;
    esp_err_t err = esp_avrc_ct_send_get_play_status_cmd(10);
    if (err != ESP_OK) {
        if (!request_error_logged) {
            ESP_LOGW(TAG, "Wiedergabestatus konnte nicht abgefragt werden: %s",
                     esp_err_to_name(err));
            request_error_logged = true;
        }
    } else {
        request_error_logged = false;
    }
}

static void metadata_to_ascii(char *dst, size_t dst_size, const uint8_t *src, size_t src_size)
{
    /* The OLED font is ASCII-only, so convert common UTF-8 characters and replace others. */
    size_t out = 0;
    for (size_t i = 0; i < src_size && out + 1 < dst_size;) {
        uint8_t c = src[i++];
        if (c < 0x80) {
            dst[out++] = c >= 0x20 ? (char)c : ' ';
        } else if (c == 0xC3 && i < src_size) {
            uint8_t next = src[i++];
            switch (next) {
            case 0xA4: dst[out++] = 'a'; break;
            case 0xB6: dst[out++] = 'o'; break;
            case 0xBC: dst[out++] = 'u'; break;
            case 0x84: dst[out++] = 'A'; break;
            case 0x96: dst[out++] = 'O'; break;
            case 0x9C: dst[out++] = 'U'; break;
            case 0x9F: dst[out++] = 's'; break;
            default:   dst[out++] = '?'; break;
            }
        } else {
            while (i < src_size && (src[i] & 0xC0) == 0x80) {
                i++;
            }
            dst[out++] = '?';
        }
    }
    dst[out] = '\0';
}

static void bt_avrc_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    /* AVRCP callback updates shared metadata/status; it does not draw to the OLED. */
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        s_avrc_connected = param->conn_stat.connected;
        ESP_LOGI(TAG, "AVRCP Fernsteuerung: %s", s_avrc_connected ? "verbunden" : "getrennt");
        if (s_avrc_connected) {
            bt_schedule_requests(true, true, false);
        } else {
            bt_schedule_requests(false, false, true);
        }
        break;
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        if (s_avrc_connected) {
            bt_schedule_requests(true, true, false);
        }
        break;
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        if (param->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE) {
            bt_schedule_requests(true, true, true);
        }
        break;
    case ESP_AVRC_CT_PLAY_STATUS_RSP_EVT:
        portENTER_CRITICAL(&s_bt_flags_mux);
        s_track_length_ms = param->play_status_rsp.song_length;
        s_track_position_ms = param->play_status_rsp.song_position;
        s_status_updated_tick = xTaskGetTickCount();
        portEXIT_CRITICAL(&s_bt_flags_mux);
        break;
    case ESP_AVRC_CT_METADATA_RSP_EVT:
        if (param->meta_rsp.attr_text != NULL && param->meta_rsp.attr_length >= 0) {
            char text[META_TEXT_SIZE];
            metadata_to_ascii(text, sizeof(text), param->meta_rsp.attr_text,
                              (size_t)param->meta_rsp.attr_length);
            if (xSemaphoreTake(s_meta_mutex, portMAX_DELAY) == pdTRUE) {
                char *destination = NULL;
                if (param->meta_rsp.attr_id == ESP_AVRC_MD_ATTR_TITLE) {
                    destination = s_title;
                } else if (param->meta_rsp.attr_id == ESP_AVRC_MD_ATTR_ARTIST) {
                    destination = s_artist;
                }
                if (destination != NULL && strcmp(destination, text) != 0) {
                    snprintf(destination, META_TEXT_SIZE, "%s", text);
                    s_meta_version++;
                }
                xSemaphoreGive(s_meta_mutex);
            } else {
                ESP_LOGE(TAG, "Metadaten-Mutex konnte nicht geschrieben werden");
            }
        }
        break;
    default:
        break;
    }
}

static void bt_send_key_with_hold(uint8_t key_code, uint32_t hold_ms)
{
    /* AVRCP passthrough commands require a press and a matching release event. */
    static uint8_t transaction_label;
    if (!s_avrc_connected) {
        ESP_LOGW(TAG, "Medientaste nicht verfuegbar: AVRCP nicht verbunden");
        return;
    }

    uint8_t press_label = transaction_label++ & 0x07;
    esp_err_t err = esp_avrc_ct_send_passthrough_cmd(press_label, key_code,
                                                      ESP_AVRC_PT_CMD_STATE_PRESSED);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Medientaste fehlgeschlagen: %s", esp_err_to_name(err));
        return;
    }

    if (hold_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(hold_ms));
    }

    uint8_t release_label = transaction_label++ & 0x07;
    err = esp_avrc_ct_send_passthrough_cmd(release_label, key_code,
                                           ESP_AVRC_PT_CMD_STATE_RELEASED);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Medientaste gesendet: 0x%02x", key_code);
    } else {
        ESP_LOGW(TAG, "Medientaste konnte nicht losgelassen werden: %s", esp_err_to_name(err));
    }
}

static void bt_seek_by_encoder(int direction, uint32_t hold_ms,
                               TickType_t *last_seek_tick,
                               TickType_t *status_deadline,
                               bool *status_pending)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t minimum_interval = pdMS_TO_TICKS(SEEK_MIN_INTERVAL_MS);
    if (*last_seek_tick != 0 &&
        (uint32_t)(now - *last_seek_tick) < minimum_interval) {
        vTaskDelay(minimum_interval - (now - *last_seek_tick));
    }

    now = xTaskGetTickCount();
    *last_seek_tick = now;
    bt_send_key_with_hold(
        direction > 0 ? ESP_AVRC_PT_CMD_FAST_FORWARD : ESP_AVRC_PT_CMD_REWIND,
        hold_ms);
    now = xTaskGetTickCount();
    s_seek_marker_direction = (int8_t)direction;
    s_seek_marker_until = now + pdMS_TO_TICKS(SEEK_MARKER_MS);
    *status_deadline = now + pdMS_TO_TICKS(SEEK_STATUS_DELAY_MS);
    *status_pending = true;
}

static void bt_send_key(uint8_t key_code)
{
    bt_send_key_with_hold(key_code, 0);
}

static void bt_toggle_play_pause(void)
{
    bt_send_key(s_playing ? ESP_AVRC_PT_CMD_PAUSE : ESP_AVRC_PT_CMD_PLAY);
}

static void bt_skip_next_track(void)
{
    bt_send_key(ESP_AVRC_PT_CMD_FORWARD);
}

static void bt_skip_previous_track(void)
{
    bt_send_key(ESP_AVRC_PT_CMD_BACKWARD);
}

static void bt_process_click_sequence(uint8_t click_count)
{
    if (click_count == 1) {
        bt_toggle_play_pause();
    } else if (click_count == 2) {
        bt_skip_next_track();
    } else if (click_count >= 3) {
        bt_skip_previous_track();
    }
}

static void process_encoder_clicks(uint8_t click_count, uint8_t page)
{
    if (page == 2 || page == 3) {
        if (click_count == 1) {
            if (page == 2) {
                s_bass_boost = 0;
                ESP_LOGI(TAG, "Bass-Boost zurueckgesetzt");
            } else {
                s_treble_boost = 0;
                ESP_LOGI(TAG, "Treble-Boost zurueckgesetzt");
            }
        } else if (click_count == 2) {
            s_wave_mode = !s_wave_mode;
            ESP_LOGI(TAG, "Visualizer-Modus: %s", s_wave_mode ? "WAVE" : "BLOCK");
        }
        return;
    }
    if (page == 4 && click_count == 2) {
        s_vocal_boost_selected = !s_vocal_boost_selected;
        ESP_LOGI(TAG, "Vocal-Seite: %s einstellen",
                 s_vocal_boost_selected ? "Vocal-Boost" :
                                          "Visualizer-Empfindlichkeit");
    } else if (page == 5 && click_count == 1) {
        s_balance = 0;
        ESP_LOGI(TAG, "Balance auf Mitte zurueckgesetzt");
    } else if (page == 6 && click_count == 1) {
        uint8_t selection = s_settings_selection;
        if (selection < SOUND_PRESET_COUNT) {
            apply_sound_preset(selection);
        } else if (selection == SOUND_PRESET_COUNT) {
            s_startup_chime = !s_startup_chime;
        } else {
            s_visualizer_fast = !s_visualizer_fast;
        }
    }
}

static void change_oled_page(int direction)
{
    s_page_forward = direction > 0;
    s_oled_page = (uint8_t)((s_oled_page + OLED_PAGE_COUNT + direction) %
                            OLED_PAGE_COUNT);
    ESP_LOGI(TAG, "OLED-Seite: %u", (unsigned)(s_oled_page + 1U));
}

/* Wird vom Bluetooth-Task aufgerufen: nur kurz in den Puffer kopieren! */
static void bt_a2d_data_cb(const uint8_t *data, uint32_t len)
{
    /* Nur ganze Bloecke schreiben, damit keine halben Stereo-Frames entstehen */
    if (xStreamBufferSpacesAvailable(s_stream) >= len) {
        if (xStreamBufferSend(s_stream, data, len, 0) != len) {
            portENTER_CRITICAL(&s_audio_stats_mux);
            s_audio_overrun_count++;
            portEXIT_CRITICAL(&s_audio_stats_mux);
        }
    } else {
        portENTER_CRITICAL(&s_audio_stats_mux);
        s_audio_overrun_count++;
        portEXIT_CRITICAL(&s_audio_stats_mux);
    }
}

static void bt_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        s_a2dp_connected = param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED;
        s_reconnect_in_progress =
            param->conn_stat.state != ESP_A2D_CONNECTION_STATE_DISCONNECTED;
        if (s_a2dp_connected) {
            memcpy(s_last_device, param->conn_stat.remote_bda, sizeof(s_last_device));
            s_last_device_valid = true;
            s_last_device_save_pending = true;
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            s_reconnect_after = xTaskGetTickCount() +
                                pdMS_TO_TICKS(RECONNECT_RETRY_DELAY_MS);
        }
        ESP_LOGI(TAG, "A2DP Verbindung: %s",
                 s_a2dp_connected ? "verbunden" : "getrennt");
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        s_playing = param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        ESP_LOGI(TAG, "Audio-Status: %d", param->audio_stat.state);
        if (s_playing) {
            bt_schedule_requests(false, true, false);
        }
        break;
    case ESP_A2D_AUDIO_CFG_EVT:
        if (param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
            /* Erstes Byte der SBC-Konfiguration: Bits 7..4 = Samplerate.
             * Per Zeiger gelesen, damit der Code nicht vom Feldnamen abhaengt
             * (in ESP-IDF 6.1 heisst "cie.sbc" nicht mehr so). */
            const uint8_t *cie = (const uint8_t *)&param->audio_cfg.mcc.cie;
            uint8_t oct0 = cie[0];
            uint32_t rate = 44100;               /* Standard, falls das Byte unplausibel ist */
            switch (oct0 & 0xF0) {
            case 0x80: rate = 16000; break;
            case 0x40: rate = 32000; break;
            case 0x20: rate = 44100; break;
            case 0x10: rate = 48000; break;
            default:   break;
            }
            s_pending_rate = rate;
        }
        break;
    default:
        break;
    }
}

static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Gekoppelt mit: %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGE(TAG, "Kopplung fehlgeschlagen, Status %d", param->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "Bluetooth Secure Simple Pairing wird bestaetigt");
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    default:
        break;
    }
}

static void bt_init(void)
{
    /* Start the Classic Bluetooth profiles used for A2DP audio and AVRCP controls. */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bluedroid_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    esp_bt_gap_register_callback(bt_gap_cb);

    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(uint8_t));
    ESP_LOGI(TAG, "Bluetooth pairing enabled without a PIN");

    ESP_ERROR_CHECK(esp_avrc_ct_register_callback(bt_avrc_cb));
    ESP_ERROR_CHECK(esp_avrc_ct_init());

    ESP_ERROR_CHECK(esp_a2d_register_callback(bt_a2d_cb));
    ESP_ERROR_CHECK(esp_a2d_sink_register_data_callback(bt_a2d_data_cb));
    ESP_ERROR_CHECK(esp_a2d_sink_init());

    esp_bt_gap_set_device_name(BT_DEVICE_NAME);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    s_bt_ready = true;

    ESP_LOGI(TAG, "Bluetooth bereit. Geraetename: %s", BT_DEVICE_NAME);
}

static void ui_task(void *arg)
{
    /* Poll shared audio/metadata state and redraw only when visible content changes. */
    esp_err_t display_err = oled_init();
    bool display_ready = display_err == ESP_OK;
    if (!display_ready) {
        ESP_LOGE(TAG, "OLED konnte nicht initialisiert werden: %s", esp_err_to_name(display_err));
    }

    uint32_t shown_meta_version = UINT32_MAX;
    uint32_t shown_progress_second = UINT32_MAX;
    bool shown_connection = false;
    bool shown_avrc_connection = false;
    bool shown_left_box_connected = false;
    bool shown_right_box_connected = false;
    bool shown_box_status_fresh = false;
    uint8_t shown_connected_box_count = UINT8_MAX;
    bool shown_playing = false;
    bool shown_clipped = false;
    bool shown_audio_active = false;
    bool shown_muted = false;
    uint8_t shown_oled_page = UINT8_MAX;
    bool shown_page_button_pressed = false;
    int8_t shown_seek_marker_direction = 0;
    int shown_volume = -1;
    int shown_bass_boost = -1;
    int shown_treble_boost = -1;
    int shown_balance = INT32_MIN;
    uint8_t shown_preset = UINT8_MAX;
    uint8_t shown_settings_selection = UINT8_MAX;
    bool shown_startup_chime = false;
    bool shown_visualizer_fast = false;
    int shown_visualizer_sensitivity = -1;
    int shown_vocal_boost = INT32_MIN;
    bool shown_vocal_boost_selected = false;
    int shown_vocal_mode = -1;
    bool shown_wave_mode = !s_wave_mode;
    uint8_t shown_spectrum[8] = {0};
    uint8_t spectrum_peaks[8] = {0};
    uint8_t shown_spectrum_peaks[8] = {0};
    uint8_t shown_vu[2] = {0};
    int8_t shown_waveform[2][WAVEFORM_SAMPLES] = {{0}};
    int8_t shown_vocal_waveform[WAVEFORM_SAMPLES] = {0};
    uint8_t wave_phase = 0;
    unsigned title_offset = 0;
    unsigned artist_offset = 0;
    TickType_t last_scroll = xTaskGetTickCount();
    TickType_t last_idle_animation = xTaskGetTickCount();
    TickType_t last_wave_animation = xTaskGetTickCount();
    uint8_t idle_animation_frame = 0;
    TickType_t last_status_request = 0;
    TickType_t settings_changed_tick = xTaskGetTickCount();
    int observed_volume = s_volume;
    int observed_bass_boost = s_bass_boost;
    int observed_treble_boost = s_treble_boost;
    int observed_vocal_boost = s_vocal_boost;
    int observed_balance = s_balance;
    bool observed_startup_chime = s_startup_chime;
    bool observed_visualizer_fast = s_visualizer_fast;
    int observed_visualizer_sensitivity = s_visualizer_sensitivity;
    int observed_vocal_mode = s_vocal_mode;
    bool settings_dirty = false;

    while (1) {
        TickType_t now = xTaskGetTickCount();
        if (s_bt_ready && s_last_device_save_pending) {
            s_last_device_save_pending = false;
            esp_err_t save_err = save_last_device();
            if (save_err != ESP_OK) {
                ESP_LOGE(TAG, "Letztes Bluetooth-Geraet konnte nicht gespeichert werden: %s",
                         esp_err_to_name(save_err));
            }
        }
        if (s_bt_ready && s_last_device_valid && !s_a2dp_connected &&
            !s_reconnect_in_progress &&
            (int32_t)(now - s_reconnect_after) >= 0) {
            esp_err_t connect_err = esp_a2d_sink_connect(s_last_device);
            s_reconnect_after = now + pdMS_TO_TICKS(RECONNECT_RETRY_DELAY_MS);
            if (connect_err == ESP_OK) {
                s_reconnect_in_progress = true;
            } else if (connect_err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "Bluetooth-Wiederverbindung fehlgeschlagen: %s",
                         esp_err_to_name(connect_err));
            }
        }
        bool request_notification;
        bool request_metadata;
        bool clear_metadata;
        bt_take_requests(&request_notification, &request_metadata, &clear_metadata);
        if (clear_metadata) {
            if (xSemaphoreTake(s_meta_mutex, portMAX_DELAY) == pdTRUE) {
                s_title[0] = '\0';
                s_artist[0] = '\0';
                s_meta_version++;
                xSemaphoreGive(s_meta_mutex);
            } else {
                ESP_LOGE(TAG, "Metadaten-Mutex konnte nicht geleert werden");
                vTaskDelete(NULL);
                return;
            }
            title_offset = 0;
            artist_offset = 0;
        }
        if (request_notification) {
            if (s_avrc_connected) {
                bt_register_track_notification();
            }
        }
        if (request_metadata) {
            if (s_avrc_connected) {
                bt_request_metadata();
            }
        }
        if (s_avrc_connected &&
            now - last_status_request >= pdMS_TO_TICKS(1000)) {
            bt_request_play_status();
            last_status_request = now;
        }

        char title[META_TEXT_SIZE];
        char artist[META_TEXT_SIZE];
        uint32_t meta_version;
        uint32_t track_length_ms;
        uint32_t track_position_ms;
        TickType_t status_updated_tick;
        if (xSemaphoreTake(s_meta_mutex, portMAX_DELAY) == pdTRUE) {
            memcpy(title, s_title, sizeof(title));
            memcpy(artist, s_artist, sizeof(artist));
            meta_version = s_meta_version;
            xSemaphoreGive(s_meta_mutex);
        } else {
            ESP_LOGE(TAG, "Metadaten-Mutex konnte nicht gelesen werden");
            vTaskDelete(NULL);
            return;
        }

        portENTER_CRITICAL(&s_bt_flags_mux);
        track_length_ms = s_track_length_ms;
        track_position_ms = s_track_position_ms;
        status_updated_tick = s_status_updated_tick;
        portEXIT_CRITICAL(&s_bt_flags_mux);

        now = xTaskGetTickCount();
        if (s_playing && track_length_ms > 0 && track_length_ms != UINT32_MAX) {
            uint32_t elapsed_since_update = (uint32_t)(now - status_updated_tick) *
                                            portTICK_PERIOD_MS;
            track_position_ms += elapsed_since_update;
            if (track_position_ms > track_length_ms) {
                track_position_ms = track_length_ms;
            }
        }
        uint32_t progress_second = track_position_ms / 1000;
        bool scroll_due = now - last_scroll >= pdMS_TO_TICKS(300);
        if (scroll_due) {
            title_offset++;
            artist_offset++;
            last_scroll = now;
        }
        int volume = s_volume;
        int balance = s_balance;
        bool connected = s_a2dp_connected;
        bool avrc_connected = s_avrc_connected;
        uint8_t box_status = 0U;
        bool box_status_fresh = link_tx_get_box_status(&box_status);
        bool left_box_connected =
            (box_status & PROTOCOL_UART_BOX_LEFT_CONNECTED) != 0U;
        bool right_box_connected =
            (box_status & PROTOCOL_UART_BOX_RIGHT_CONNECTED) != 0U;
        uint8_t connected_box_count =
            (left_box_connected ? 1U : 0U) +
            (right_box_connected ? 1U : 0U);
        bool playing = s_playing;
        uint8_t oled_page = s_oled_page;
        bool wave_mode = s_wave_mode;
        bool vocal_boost_selected = s_vocal_boost_selected;
        int vocal_mode = s_vocal_mode;
        uint8_t preset = s_preset_index;
        int bass_boost = s_bass_boost;
        int treble_boost = s_treble_boost;
        int vocal_boost = s_vocal_boost;
        bool clipped = (int32_t)(s_clip_until - now) > 0;
        bool audio_active = (int32_t)(s_audio_active_until - now) > 0;
        int visualizer_sensitivity = s_visualizer_sensitivity;
        bool page_button_pressed = s_page_button_pressed;
        int8_t seek_marker_direction =
            (int32_t)(s_seek_marker_until - now) > 0
                ? s_seek_marker_direction : 0;
        if (volume != observed_volume || bass_boost != observed_bass_boost ||
            treble_boost != observed_treble_boost ||
            vocal_boost != observed_vocal_boost || balance != observed_balance ||
            s_startup_chime != observed_startup_chime ||
            s_visualizer_fast != observed_visualizer_fast ||
            visualizer_sensitivity != observed_visualizer_sensitivity ||
            vocal_mode != observed_vocal_mode) {
            settings_dirty = true;
            settings_changed_tick = now;
            observed_volume = volume;
            observed_bass_boost = bass_boost;
            observed_treble_boost = treble_boost;
            observed_vocal_boost = vocal_boost;
            observed_balance = balance;
            observed_startup_chime = s_startup_chime;
            observed_visualizer_fast = s_visualizer_fast;
            observed_visualizer_sensitivity = visualizer_sensitivity;
            observed_vocal_mode = vocal_mode;
        }
        if (settings_dirty &&
            now - settings_changed_tick >= pdMS_TO_TICKS(2000)) {
            esp_err_t save_err = save_audio_settings(volume, bass_boost, treble_boost,
                                                      vocal_boost, balance, s_startup_chime,
                                                      s_visualizer_fast,
                                                      visualizer_sensitivity,
                                                      vocal_mode);
            if (save_err == ESP_OK) {
                settings_dirty = false;
                ESP_LOGI(TAG, "Audio-Einstellungen gespeichert");
            } else {
                ESP_LOGE(TAG, "Audio-Einstellungen konnten nicht gespeichert werden: %s",
                         esp_err_to_name(save_err));
                settings_changed_tick = now;
            }
        }
        if ((uint32_t)(now - s_last_input_tick) >= pdMS_TO_TICKS(OLED_IDLE_TIMEOUT_MS) &&
            oled_page != 0 && !page_button_pressed) {
            s_page_forward = false;
            s_oled_page = 0;
            continue;
        }
        uint8_t spectrum[8];
        uint8_t vu[2];
        int8_t waveform[2][WAVEFORM_SAMPLES];
        int8_t vocal_waveform[WAVEFORM_SAMPLES];
        portENTER_CRITICAL(&s_visualizer_mux);
        for (size_t band = 0; band < 8; band++) {
            spectrum[band] = s_spectrum_levels[band];
        }
        for (size_t channel = 0; channel < 2; channel++) {
            vu[channel] = s_vu_levels[channel];
            for (size_t sample = 0; sample < WAVEFORM_SAMPLES; sample++) {
                waveform[channel][sample] = s_waveform[channel][sample];
            }
        }
        for (size_t sample = 0; sample < WAVEFORM_SAMPLES; sample++) {
            vocal_waveform[sample] = s_vocal_waveform[sample];
        }
        portEXIT_CRITICAL(&s_visualizer_mux);
        bool vu_changed = vu[0] != shown_vu[0] || vu[1] != shown_vu[1];
        bool show_vu = audio_active || vu[0] > 4 || vu[1] > 4;
        bool idle_animation_due = false;
        if (oled_page == 0 && !show_vu &&
            now - last_idle_animation >= pdMS_TO_TICKS(150)) {
            idle_animation_frame++;
            last_idle_animation = now;
            idle_animation_due = true;
        }
        bool wave_page = oled_page == 2 || oled_page == 3;
        bool visualizer_page = wave_page || oled_page == 4;
        bool spectrum_changed = false;
        bool peaks_changed = false;
        bool waveform_changed = false;
        bool vocal_waveform_changed = false;
        if (visualizer_page) {
            for (size_t band = 0; band < 8; band++) {
                /* Keep a decaying peak marker for the block visualizers. */
                if (spectrum[band] >= spectrum_peaks[band]) {
                    spectrum_peaks[band] = spectrum[band];
                } else if (spectrum_peaks[band] > 0) {
                    spectrum_peaks[band]--;
                }
                if (spectrum[band] != shown_spectrum[band]) {
                    spectrum_changed = true;
                }
                if (spectrum_peaks[band] != shown_spectrum_peaks[band]) {
                    peaks_changed = true;
                }
            }
            if (wave_mode) {
                waveform_changed = memcmp(waveform, shown_waveform,
                                          sizeof(waveform)) != 0;
            }
            if (oled_page == 4) {
                vocal_waveform_changed = memcmp(vocal_waveform,
                                                shown_vocal_waveform,
                                                sizeof(vocal_waveform)) != 0;
            }
        }
        bool wave_animation_due = false;
        if (wave_page &&
            now - last_wave_animation >= pdMS_TO_TICKS(100)) {
            /* Slowly move the decorative wave so the trace stays animated in silence. */
            wave_phase++;
            last_wave_animation = now;
            wave_animation_due = true;
        }
        bool changed = connected != shown_connection ||
                       avrc_connected != shown_avrc_connection ||
                       (oled_page == 0 &&
                        (left_box_connected != shown_left_box_connected ||
                         right_box_connected != shown_right_box_connected)) ||
                       (oled_page == 7 &&
                        (box_status_fresh != shown_box_status_fresh ||
                         connected_box_count != shown_connected_box_count)) ||
                       playing != shown_playing || clipped != shown_clipped ||
                       s_muted != shown_muted ||
                       page_button_pressed != shown_page_button_pressed ||
                       (oled_page == 1 &&
                        seek_marker_direction != shown_seek_marker_direction) ||
                       (oled_page == 0 && audio_active != shown_audio_active) ||
                       (oled_page == 0 && (vu_changed || idle_animation_due)) ||
                       volume != shown_volume || meta_version != shown_meta_version ||
                       oled_page != shown_oled_page || scroll_due ||
                       progress_second != shown_progress_second ||
                       (oled_page == 2 && (bass_boost != shown_bass_boost ||
                                           spectrum_changed || peaks_changed ||
                                           waveform_changed || wave_animation_due)) ||
                       (oled_page == 3 && (treble_boost != shown_treble_boost ||
                                           spectrum_changed || peaks_changed ||
                                           waveform_changed || wave_animation_due)) ||
                       (wave_page && wave_mode != shown_wave_mode) ||
                       (visualizer_page && visualizer_sensitivity !=
                                           shown_visualizer_sensitivity) ||
                       (oled_page == 4 && (spectrum_changed || peaks_changed ||
                                           vocal_waveform_changed ||
                                           vocal_boost != shown_vocal_boost ||
                                           vocal_boost_selected !=
                                               shown_vocal_boost_selected ||
                                           vocal_mode != shown_vocal_mode)) ||
                       (oled_page == 6 && (preset != shown_preset ||
                                           s_settings_selection != shown_settings_selection ||
                                           s_startup_chime != shown_startup_chime ||
                                           s_visualizer_fast != shown_visualizer_fast)) ||
                       (oled_page == 5 && balance != shown_balance);
        if (display_ready && changed) {
            bool page_transition = shown_oled_page != UINT8_MAX &&
                                   oled_page != shown_oled_page;
            oled_clear();
            if (oled_page == 0) {
                oled_text(0, 0, connected ? "BT+" : "BT-", 1);
                oled_text(22, 0, avrc_connected ? "CTRL+" : "CTRL-", 1);
                oled_text(56, 0, left_box_connected ? "L+" : "L-", 1);
                oled_text(68, 0, right_box_connected ? "R+" : "R-", 1);
                oled_text(80, 0, "1/8", 1);
                oled_text(102, 0, clipped ? "CLIP" : (s_muted ? "MUTE" :
                          (playing ? "PLAY" : "PAUS")), 1);
            } else if (oled_page == 1) {
                oled_text(0, 0, "PROGRESS", 1);
            } else if (oled_page == 2) {
                oled_text(0, 0, "BASS", 1);
            } else if (oled_page == 3) {
                oled_text(0, 0, "TREBLE", 1);
            } else if (oled_page == 4) {
                oled_text(0, 0, "VOCAL", 1);
            } else if (oled_page == 5) {
                oled_text(0, 0, "BALANCE", 1);
            } else if (oled_page == 6) {
                oled_text(0, 0, "SETUP", 1);
            } else if (oled_page == 7) {
                oled_text(0, 0, "BOX STATUS", 1);
            }
            if (oled_page != 0) {
                char page_label[8];
                snprintf(page_label, sizeof(page_label), "%u/8",
                         (unsigned)(oled_page + 1U));
                if (clipped) {
                    oled_text(96, 0, "!", 1);
                }
                oled_text(108, 0, page_label, 1);
            }
            if (page_button_pressed && oled_page >= 1 && oled_page <= 6) {
                oled_text(78, 0, "SET", 1);
            }
            if (oled_page == 1 && seek_marker_direction != 0) {
                oled_text(96, 14, seek_marker_direction > 0 ? ">>" : "<<", 1);
            }
            oled_fill_rect(0, 10, 128, 1, true);

            if (oled_page == 0) {
                if (title[0] == '\0') {
                    oled_text(0, 14, "NO TITLE", 2);
                } else {
                    oled_text_window(0, 14, title, 2, 10, title_offset);
                }
                if (artist[0] == '\0') {
                    oled_text(0, 34, "UNKNOWN ARTIST", 1);
                } else {
                    oled_text_window(0, 34, artist, 1, 21, artist_offset);
                }
                char volume_text[16];
                snprintf(volume_text, sizeof(volume_text), "VOL %d%%", volume);
                oled_text(0, 42, s_muted ? "MUTE" : volume_text, 1);
                oled_text(72, 42, audio_active ? "AUDIO IN" : "NO SIGNAL", 1);
                if (show_vu) {
                    oled_rect(8, 49, 52, 6, true);
                    oled_fill_rect(10, 51, (48 * vu[0]) / 100, 2, true);
                    oled_rect(74, 49, 52, 6, true);
                    oled_fill_rect(76, 51, (48 * vu[1]) / 100, 2, true);
                } else {
                    for (int bar = 0; bar < 8; bar++) {
                        int phase = (idle_animation_frame + (uint8_t)(bar * 3)) % 12;
                        int height = 1 + (phase <= 6 ? phase : 12 - phase) / 2;
                        oled_fill_rect(8 + bar * 15, 55 - height, 8, height, true);
                    }
                }
                oled_rect(0, 57, 128, 7, true);
                oled_fill_rect(2, 59, (124 * volume) / VOLUME_MAX, 3, true);
            } else if (oled_page == 1) {
                if (track_length_ms > 0 && track_length_ms != UINT32_MAX) {
                    uint32_t remaining_ms = track_length_ms - track_position_ms;
                    uint32_t remaining_seconds = remaining_ms / 1000;
                    char time_left[16];
                    snprintf(time_left, sizeof(time_left), "-%02lu:%02lu",
                             (unsigned long)(remaining_seconds / 60),
                             (unsigned long)(remaining_seconds % 60));
                    oled_text(20, 14, time_left, 2);
                    if (title[0] != '\0') {
                        oled_text_window(0, 31, title, 1, 21, title_offset);
                    }
                    oled_rect(0, 42, 128, 8, true);
                    uint32_t progress_width =
                        (uint32_t)(((uint64_t)track_position_ms * 124) / track_length_ms);
                    oled_fill_rect(2, 44, (int)progress_width, 4, true);

                    uint32_t elapsed_seconds = track_position_ms / 1000;
                    uint32_t total_seconds = track_length_ms / 1000;
                    char elapsed_text[12];
                    char total_text[12];
                    snprintf(elapsed_text, sizeof(elapsed_text), "%lu:%02lu",
                             (unsigned long)(elapsed_seconds / 60),
                             (unsigned long)(elapsed_seconds % 60));
                    snprintf(total_text, sizeof(total_text), "%lu:%02lu",
                             (unsigned long)(total_seconds / 60),
                             (unsigned long)(total_seconds % 60));
                    oled_text(0, 54, elapsed_text, 1);
                    oled_text(92, 54, total_text, 1);
                } else {
                    oled_text(12, 20, "TIME UNAVAILABLE", 1);
                    oled_text(0, 54, connected ? "WAITING FOR PLAYER" : "NOT CONNECTED", 1);
                }
            } else if (oled_page == 2 || oled_page == 3) {
                char boost_text[24];
                bool treble_page = oled_page == 3;
                int tone_boost = treble_page ? treble_boost : bass_boost;
                const char *tone_name = treble_page ? "TREBLE" : "BASS";
                char gain_text[12];
                if (tone_boost == 0) {
                    snprintf(boost_text, sizeof(boost_text), "%s NORMAL", tone_name);
                } else {
                    format_eq_gain(gain_text, sizeof(gain_text), tone_boost,
                                   treble_page ? EQ_DB_PER_CONTROL_UNIT
                                               : BASS_EQ_DB_PER_CONTROL_UNIT);
                    snprintf(boost_text, sizeof(boost_text), "%s %sdB",
                             tone_name, gain_text);
                }
                oled_text(0, 14, boost_text, 1);
                oled_text(96, 14, wave_mode ? "WAVE" : "BLOCK", 1);
                if (wave_mode) {
                    /* Overlay sampled stereo audio on a gentle animated carrier wave. */
                    oled_text(0, 27, "L", 1);
                    oled_text(0, 46, "R", 1);
                    const int center_y[2] = {34, 51};
                    int previous_y[2] = {34, 51};
                    for (int x = 8; x < 128; x += 5) {
                        oled_fill_rect(x, 34, 2, 1, true);
                        oled_fill_rect(x, 51, 2, 1, true);
                    }
                    for (int sample = 0; sample < WAVEFORM_SAMPLES; sample++) {
                        int x = 8 + sample * 119 / (WAVEFORM_SAMPLES - 1);
                        int band = sample * 8 / WAVEFORM_SAMPLES;
                        int level = spectrum[band];
                        int shape_index = ((int)(sample / 2) + wave_phase) & 31;
                        int wave_height = 1 + (level * 3) / 100;
                        int y[2];
                        for (size_t channel = 0; channel < 2; channel++) {
                            int signal_offset =
                                (waveform[channel][sample] * 8 *
                                 visualizer_sensitivity) / 12800;
                            int shaped_offset =
                                (s_wave_shape[shape_index] * wave_height) / 10;
                            y[channel] = center_y[channel] - signal_offset -
                                         shaped_offset;
                            int min_y = channel == 0 ? 26 : 44;
                            int max_y = channel == 0 ? 42 : 59;
                            if (y[channel] < min_y) y[channel] = min_y;
                            if (y[channel] > max_y) y[channel] = max_y;
                            if (sample > 0) {
                                int previous_x = 8 + (sample - 1) * 119 /
                                                     (WAVEFORM_SAMPLES - 1);
                                ui_draw_line(previous_x, previous_y[channel],
                                             x, y[channel]);
                            }
                            previous_y[channel] = y[channel];
                        }
                    }
                } else {
                    /* Block mode draws eight spectrum bands and their peak-hold markers. */
                    const int bar_width = 11;
                    const int bar_gap = 4;
                    const int baseline = 58;
                    const int max_height = 32;
                    for (int bar = 0; bar < 8; bar++) {
                        int height = (spectrum[bar] * max_height) / 100;
                        int x = 4 + bar * (bar_width + bar_gap);
                        for (int segment = 0; segment < 8; segment++) {
                            int segment_height = 3;
                            int segment_y = baseline - (segment + 1) * 4;
                            if (height >= (segment + 1) * 4 - 1) {
                                oled_fill_rect(x, segment_y, bar_width,
                                               segment_height, true);
                            }
                        }
                        int peak_y = baseline - (spectrum_peaks[bar] * max_height) / 100;
                        if (spectrum_peaks[bar] > 0 && peak_y >= 24 && peak_y < baseline) {
                            oled_fill_rect(x, peak_y, bar_width, 1, true);
                        }
                    }
                }
            } else if (oled_page == 6) {
                uint8_t selection = s_settings_selection;
                oled_rect(3, 15, 122, 45, true);
                if (selection < SOUND_PRESET_COUNT) {
                    char preset_title[20];
                    char preset_text[32];
                    char bass_gain[12];
                    char treble_gain[12];
                    snprintf(preset_title, sizeof(preset_title), "PRESET %u/6",
                             (unsigned)(selection + 1U));
                    format_eq_gain(bass_gain, sizeof(bass_gain),
                                   s_preset_bass[selection],
                                   BASS_EQ_DB_PER_CONTROL_UNIT);
                    format_eq_gain(treble_gain, sizeof(treble_gain),
                                   s_preset_treble[selection],
                                   EQ_DB_PER_CONTROL_UNIT);
                    snprintf(preset_text, sizeof(preset_text), "B%s T%s dB",
                             bass_gain, treble_gain);
                    oled_text(10, 22, preset_title, 1);
                    oled_text(10, 34, s_preset_names[selection], 2);
                    oled_text(10, 52, preset_text, 1);
                } else if (selection == SOUND_PRESET_COUNT) {
                    oled_text(10, 22, "STARTUP CHIME", 1);
                    oled_text(10, 37, s_startup_chime ? "ON" : "OFF", 2);
                } else {
                    oled_text(10, 22, "VISUALIZER MODE", 1);
                    oled_text(10, 37, s_visualizer_fast ? "FAST" : "SMOOTH", 2);
                }
            } else if (oled_page == 7) {
                oled_text(8, 16, "CONNECTED BOXES", 1);
                if (box_status_fresh) {
                    char count_text[8];
                    snprintf(count_text, sizeof(count_text), "%u / 2",
                             (unsigned)connected_box_count);
                    oled_text(40, 27, count_text, 2);
                    oled_text(8, 50, left_box_connected ? "LEFT: ON" :
                              "LEFT: OFF", 1);
                    oled_text(72, 50, right_box_connected ? "RIGHT: ON" :
                              "RIGHT: OFF", 1);
                } else {
                    oled_text(40, 27, "-- / 2", 2);
                    oled_text(8, 50, "LEFT: --", 1);
                    oled_text(72, 50, "RIGHT: --", 1);
                }
            } else if (oled_page == 5) {
                    char balance_text[20];
                    snprintf(balance_text, sizeof(balance_text), "%s %d%%",
                             balance < 0 ? "LEFT" : "RIGHT",
                             balance < 0 ? -balance : balance);
                    if (balance == 0) {
                        snprintf(balance_text, sizeof(balance_text), "CENTER");
                    }
                    oled_text(0, 16, balance_text, 1);
                    oled_text(8, 32, "L", 1);
                    oled_text(112, 32, "R", 1);
                    oled_rect(20, 32, 88, 9, true);
                    int marker_x = 22 + ((balance + 100) * 84) / 200;
                    oled_fill_rect(marker_x, 34, 3, 5, true);
                    oled_text(0, 50, "TURN: BALANCE", 1);
            } else if (oled_page == 4) {
                /* Vocal page combines the extracted voice-band waveform with band levels. */
                char control_text[20];
                if (vocal_boost_selected) {
                    char gain_text[12];
                    format_eq_gain(gain_text, sizeof(gain_text), vocal_boost,
                                   EQ_DB_PER_CONTROL_UNIT);
                    snprintf(control_text, sizeof(control_text), "VOICE %sdB",
                             gain_text);
                } else {
                    snprintf(control_text, sizeof(control_text), "SENS %d%%",
                             visualizer_sensitivity);
                }
                oled_text(0, 16, control_text, 1);
                oled_text(80, 16,
                          vocal_mode == VOCAL_MODE_ISOLATE ? "ISO" :
                          vocal_mode == VOCAL_MODE_REMOVE ? "REMOVE" : "OFF", 1);
                for (int x = 5; x < 128; x += 5) {
                    oled_fill_rect(x, 36, 2, 1, true);
                }
                int previous_y = 36;
                for (int sample = 0; sample < WAVEFORM_SAMPLES; sample++) {
                    int x = 5 + sample * 122 / (WAVEFORM_SAMPLES - 1);
                    int signal_offset = (vocal_waveform[sample] * 8 *
                                         visualizer_sensitivity) / 12800;
                    int y = 36 - signal_offset;
                    if (y < 28) y = 28;
                    if (y > 44) y = 44;
                    if (sample > 0) {
                        int previous_x = 5 + (sample - 1) * 122 /
                                             (WAVEFORM_SAMPLES - 1);
                        ui_draw_line(previous_x, previous_y, x, y);
                    }
                    previous_y = y;
                }
                const int bar_width = 14;
                const int bar_gap = 10;
                const int baseline = 53;
                for (int bar = 0; bar < 5; bar++) {
                    int band = bar + 1;
                    int height = (spectrum[band] * 8) / 100;
                    int x = 9 + bar * (bar_width + bar_gap);
                    if (height > 0) {
                        oled_fill_rect(x, baseline - height, bar_width,
                                       height, true);
                    }
                    int peak_y = baseline - (spectrum_peaks[band] * 8) / 100;
                    if (spectrum_peaks[band] > 0 && peak_y >= 44 &&
                        peak_y < baseline) {
                        oled_fill_rect(x, peak_y, bar_width, 1, true);
                    }
                }
                oled_text(6, 56, "HOLD ISO  2X SEL", 1);
            }
            esp_err_t err = page_transition
                ? oled_flush_slide(s_page_forward)
                : oled_flush();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "OLED-Aktualisierung fehlgeschlagen: %s", esp_err_to_name(err));
                display_ready = false;
            }
        }
        shown_connection = connected;
        shown_avrc_connection = avrc_connected;
        shown_left_box_connected = left_box_connected;
        shown_right_box_connected = right_box_connected;
        shown_box_status_fresh = box_status_fresh;
        shown_connected_box_count = connected_box_count;
        shown_playing = playing;
        shown_clipped = clipped;
        shown_audio_active = audio_active;
        shown_muted = s_muted;
        shown_page_button_pressed = page_button_pressed;
        shown_seek_marker_direction = seek_marker_direction;
        shown_volume = volume;
        shown_meta_version = meta_version;
        shown_oled_page = oled_page;
        shown_progress_second = progress_second;
        shown_bass_boost = bass_boost;
        shown_treble_boost = treble_boost;
        shown_balance = balance;
        shown_preset = preset;
        shown_settings_selection = s_settings_selection;
        shown_startup_chime = s_startup_chime;
        shown_visualizer_fast = s_visualizer_fast;
        shown_visualizer_sensitivity = visualizer_sensitivity;
        shown_vocal_boost = vocal_boost;
        shown_vocal_boost_selected = vocal_boost_selected;
        shown_vocal_mode = vocal_mode;
        shown_wave_mode = wave_mode;
        memcpy(shown_spectrum, spectrum, sizeof(shown_spectrum));
        memcpy(shown_spectrum_peaks, spectrum_peaks, sizeof(shown_spectrum_peaks));
        memcpy(shown_waveform, waveform, sizeof(shown_waveform));
        memcpy(shown_vu, vu, sizeof(shown_vu));
        send_dsp_params_to_hub();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---------- Drehencoder ---------- */
static void encoder_task(void *arg)
{
    /*
     * Encoder 1 rotation always adjusts volume and its button always controls
     * playback. Encoder 2 navigates pages when released and adjusts the
     * current page while pressed; its button also handles page-specific clicks.
     */
    gpio_config_t encoder_io = {
         .pin_bit_mask = (1ULL << ENC_CLK_PIN) | (1ULL << ENC_DT_PIN) |
                         (1ULL << PAGE_ENC_CLK_PIN) | (1ULL << PAGE_ENC_DT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&encoder_io));

    gpio_config_t button_io = {
        .pin_bit_mask = (1ULL << ENC_SW_PIN) | (1ULL << PAGE_ENC_SW_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button_io));

    portENTER_CRITICAL(&s_encoder_mux);
    s_encoder_state = (uint8_t)((gpio_get_level(ENC_CLK_PIN) << 1) |
                                 gpio_get_level(ENC_DT_PIN));
    s_encoder_delta = 0;
    s_encoder_edge_mask = 0;
    portEXIT_CRITICAL(&s_encoder_mux);
    portENTER_CRITICAL(&s_page_encoder_mux);
    s_page_encoder_state = (uint8_t)((gpio_get_level(PAGE_ENC_CLK_PIN) << 1) |
                                     gpio_get_level(PAGE_ENC_DT_PIN));
    s_page_encoder_delta = 0;
    portEXIT_CRITICAL(&s_page_encoder_mux);
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(ENC_CLK_PIN, encoder_gpio_isr,
                                         (void *)(intptr_t)ENC_CLK_PIN));
    ESP_ERROR_CHECK(gpio_isr_handler_add(ENC_DT_PIN, encoder_gpio_isr,
                                         (void *)(intptr_t)ENC_DT_PIN));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PAGE_ENC_CLK_PIN, page_encoder_gpio_isr,
                                         (void *)(intptr_t)PAGE_ENC_CLK_PIN));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PAGE_ENC_DT_PIN, page_encoder_gpio_isr,
                                         (void *)(intptr_t)PAGE_ENC_DT_PIN));

    int acc = 0;
    int page_acc = 0;
#if ENC_DEBUG
    int debug_clk = gpio_get_level(ENC_CLK_PIN);
    int debug_dt = gpio_get_level(ENC_DT_PIN);
    int debug_sw = gpio_get_level(ENC_SW_PIN);
    ESP_LOGI(TAG, "Encoder pins: A/CLK(GPIO%d)=%d B/DT(GPIO%d)=%d SW(GPIO%d)=%d",
             ENC_CLK_PIN, debug_clk, ENC_DT_PIN, debug_dt, ENC_SW_PIN, debug_sw);
#endif

    int btn_last = 1;
    int btn_stable = 1;
    int btn_count = 0;
    int media_btn_last = 1;
    int media_btn_stable = 1;
    int media_btn_count = 0;
    TickType_t button_down_tick = 0;
    bool turned_while_pressed = false;
    bool click_pending = false;
    uint8_t click_count = 0;
    uint8_t click_page = 0;
    TickType_t click_deadline = 0;
    bool media_click_pending = false;
    uint8_t media_click_count = 0;
    TickType_t media_click_deadline = 0;
    int page_button_raw_last = gpio_get_level(PAGE_ENC_SW_PIN);
    TickType_t page_button_edge_tick = xTaskGetTickCount();
    TickType_t last_seek_tick = 0;
    TickType_t seek_status_deadline = 0;
    bool seek_status_pending = false;
    s_page_button_pressed = page_button_raw_last == 0;

    while (1) {
        int encoder_delta;
#if ENC_DEBUG
        uint8_t edge_mask;
#endif
        portENTER_CRITICAL(&s_encoder_mux);
        encoder_delta = s_encoder_delta;
        s_encoder_delta = 0;
#if ENC_DEBUG
        edge_mask = s_encoder_edge_mask;
#endif
        s_encoder_edge_mask = 0;
        portEXIT_CRITICAL(&s_encoder_mux);
        acc += encoder_delta;
        if (encoder_delta != 0) {
            s_last_input_tick = xTaskGetTickCount();
        }

        int page_encoder_delta;
        portENTER_CRITICAL(&s_page_encoder_mux);
        page_encoder_delta = s_page_encoder_delta;
        s_page_encoder_delta = 0;
        portEXIT_CRITICAL(&s_page_encoder_mux);
        TickType_t now = xTaskGetTickCount();
        int page_button_raw = gpio_get_level(PAGE_ENC_SW_PIN);
        if (page_button_raw != page_button_raw_last) {
            page_button_raw_last = page_button_raw;
            page_button_edge_tick = now;
            page_acc = 0;
            s_page_button_pressed = page_button_raw == 0;
            if (page_button_raw == 0) {
                turned_while_pressed = false;
                button_down_tick = now;
            }
        }
        page_acc += page_encoder_delta;
        if (page_encoder_delta != 0) {
            s_last_input_tick = now;
        }

#if ENC_DEBUG
        int clk = gpio_get_level(ENC_CLK_PIN);
        int dt = gpio_get_level(ENC_DT_PIN);
        int sw = gpio_get_level(ENC_SW_PIN);
        if (edge_mask != 0 || clk != debug_clk || dt != debug_dt || sw != debug_sw) {
            ESP_LOGI(TAG, "PINS: A/CLK(GPIO%d)=%d B/DT(GPIO%d)=%d SW(GPIO%d)=%d edges=%c%c",
                     ENC_CLK_PIN, clk, ENC_DT_PIN, dt, ENC_SW_PIN, sw,
                     (edge_mask & 1) ? 'A' : '-',
                     (edge_mask & 2) ? 'B' : '-');
            debug_clk = clk;
            debug_dt = dt;
            debug_sw = sw;
        }
#endif

        if (acc >= 4) {                 /* eine Raststellung rechts = lauter */
            acc -= 4;
            int old_volume = s_volume;
            int v = old_volume + VOLUME_STEP;
            s_volume = v > VOLUME_MAX ? VOLUME_MAX : v;
            if (s_volume != old_volume) ESP_LOGI(TAG, "Lautstaerke: %d", s_volume);
        } else if (acc <= -4) {         /* links = leiser */
            acc += 4;
            int old_volume = s_volume;
            int v = old_volume - VOLUME_STEP;
            s_volume = v < 0 ? 0 : v;
            if (s_volume != old_volume) ESP_LOGI(TAG, "Lautstaerke: %d", s_volume);
        }

        while (page_acc >= 4 || page_acc <= -4) {
            now = xTaskGetTickCount();
            page_button_raw = gpio_get_level(PAGE_ENC_SW_PIN);
            if (page_button_raw != page_button_raw_last) {
                page_button_raw_last = page_button_raw;
                page_button_edge_tick = now;
                page_acc = 0;
                s_page_button_pressed = page_button_raw == 0;
                if (page_button_raw == 0) {
                    turned_while_pressed = false;
                    button_down_tick = now;
                }
                break;
            }
            if ((uint32_t)(now - page_button_edge_tick) <
                pdMS_TO_TICKS(BUTTON_EDGE_GUARD_MS)) {
                page_acc = 0;
                break;
            }

            int direction = page_acc > 0 ? 1 : -1;
            page_acc -= direction * 4;
            s_last_input_tick = now;
            if (page_button_raw == 0) {
                turned_while_pressed = true;
                click_pending = false;
                click_count = 0;

                switch (s_oled_page) {
                case 1:
                    bt_seek_by_encoder(direction, SEEK_HOLD_MS,
                                       &last_seek_tick, &seek_status_deadline,
                                       &seek_status_pending);
                    break;
                case 2:
                    s_bass_boost += direction * BASS_BOOST_STEP;
                    if (s_bass_boost < BASS_BOOST_MIN) s_bass_boost = BASS_BOOST_MIN;
                    if (s_bass_boost > BASS_BOOST_MAX) s_bass_boost = BASS_BOOST_MAX;
                    break;
                case 3:
                    s_treble_boost += direction * TREBLE_BOOST_STEP;
                    if (s_treble_boost < TREBLE_BOOST_MIN) {
                        s_treble_boost = TREBLE_BOOST_MIN;
                    }
                    if (s_treble_boost > TREBLE_BOOST_MAX) {
                        s_treble_boost = TREBLE_BOOST_MAX;
                    }
                    break;
                case 4:
                    if (s_vocal_boost_selected) {
                        s_vocal_boost += direction * VOCAL_BOOST_STEP;
                        if (s_vocal_boost < VOCAL_BOOST_MIN) {
                            s_vocal_boost = VOCAL_BOOST_MIN;
                        }
                        if (s_vocal_boost > VOCAL_BOOST_MAX) {
                            s_vocal_boost = VOCAL_BOOST_MAX;
                        }
                    } else {
                        s_visualizer_sensitivity +=
                            direction * VIZ_SENSITIVITY_STEP;
                        if (s_visualizer_sensitivity < VIZ_SENSITIVITY_MIN) {
                            s_visualizer_sensitivity = VIZ_SENSITIVITY_MIN;
                        }
                        if (s_visualizer_sensitivity > VIZ_SENSITIVITY_MAX) {
                            s_visualizer_sensitivity = VIZ_SENSITIVITY_MAX;
                        }
                    }
                    break;
                case 5:
                    s_balance += direction * BALANCE_STEP;
                    if (s_balance < BALANCE_MIN) s_balance = BALANCE_MIN;
                    if (s_balance > BALANCE_MAX) s_balance = BALANCE_MAX;
                    break;
                case 6:
                    s_settings_selection = (uint8_t)(
                        (s_settings_selection + SOUND_PRESET_COUNT + 2U +
                         direction) % (SOUND_PRESET_COUNT + 2U));
                    break;
                default:
                    break;
                }
            } else {
                click_pending = false;
                click_count = 0;
                change_oled_page(direction);
            }
        }

        if (seek_status_pending &&
            (int32_t)(now - seek_status_deadline) >= 0) {
            seek_status_pending = false;
            if (s_avrc_connected) {
                bt_request_play_status();
            }
        }

        /* Taster entprellen (ca. 50 ms bei 100-Hz-FreeRTOS-Takt) */
        int btn = gpio_get_level(PAGE_ENC_SW_PIN);
        if (btn == btn_last) {
            if (btn_count < 5) btn_count++;
            if (btn_count == 5 && btn != btn_stable) {
                btn_stable = btn;
                now = xTaskGetTickCount();
                s_last_input_tick = now;
                if (btn_stable == 0) {
                    button_down_tick = page_button_edge_tick;
                } else if (turned_while_pressed) {
                    turned_while_pressed = false;
                    click_pending = false;
                    click_count = 0;
                } else if ((uint32_t)(now - button_down_tick) >=
                           pdMS_TO_TICKS(LONG_PRESS_MS)) {
                    click_pending = false;
                    click_count = 0;
                    if (s_oled_page == 4) {
                        s_vocal_mode = (s_vocal_mode + 1) % 3;
                        static const char *mode_names[] = {
                            "Aus", "Isolation", "Entfernen"
                        };
                        ESP_LOGI(TAG, "Vocal-Modus: %s",
                                 mode_names[s_vocal_mode]);
                    } else {
                        s_muted = !s_muted;
                        ESP_LOGI(TAG, "Audio %s",
                                 s_muted ? "stummgeschaltet" : "Ton wieder an");
                    }
                } else {
                    if (click_pending &&
                        click_page == s_oled_page &&
                        (int32_t)(click_deadline - now) >= 0) {
                        if (click_count < 20) click_count++;
                        click_deadline = now + pdMS_TO_TICKS(DOUBLE_CLICK_MS);
                    } else {
                        if (click_pending) {
                            process_encoder_clicks(click_count, click_page);
                        }
                        click_pending = true;
                        click_count = 1;
                        click_page = s_oled_page;
                        click_deadline = now + pdMS_TO_TICKS(DOUBLE_CLICK_MS);
                    }
                }
            }
        } else {
            btn_count = 0;
        }
        btn_last = btn;

        now = xTaskGetTickCount();
        if (click_pending && (int32_t)(now - click_deadline) >= 0) {
            click_pending = false;
            process_encoder_clicks(click_count, click_page);
            click_count = 0;
        }

        int media_btn = gpio_get_level(ENC_SW_PIN);
        if (media_btn == media_btn_last) {
            if (media_btn_count < 5) media_btn_count++;
            if (media_btn_count == 5 && media_btn != media_btn_stable) {
                media_btn_stable = media_btn;
                s_last_input_tick = xTaskGetTickCount();
                if (media_btn_stable == 1) {
                    TickType_t now = xTaskGetTickCount();
                    if (media_click_pending &&
                        (int32_t)(media_click_deadline - now) >= 0) {
                        if (media_click_count < 20) {
                            media_click_count++;
                        }
                        media_click_deadline = now + pdMS_TO_TICKS(DOUBLE_CLICK_MS);
                    } else {
                        if (media_click_pending) {
                            bt_process_click_sequence(media_click_count);
                        }
                        media_click_pending = true;
                        media_click_count = 1;
                        media_click_deadline = now + pdMS_TO_TICKS(DOUBLE_CLICK_MS);
                    }
                }
            }
        } else {
            media_btn_count = 0;
        }
        media_btn_last = media_btn;

        if (media_click_pending &&
            (int32_t)(xTaskGetTickCount() - media_click_deadline) >= 0) {
            media_click_pending = false;
            bt_process_click_sequence(media_click_count);
            media_click_count = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ---------- main ---------- */
void app_main(void)
{
    /* Create shared buffers/tasks first, then bring up audio, controls, and Bluetooth. */
    ESP_LOGI(TAG, "ESP A Bluetooth speaker ready, firmware: %s",
             FIRMWARE_VERSION_NAME);
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    load_audio_settings();
    s_last_input_tick = xTaskGetTickCount();

    s_stream = xStreamBufferCreate(STREAM_BUF_SIZE, 1);
    assert(s_stream);
    s_meta_mutex = xSemaphoreCreateMutex();
    assert(s_meta_mutex);

    esp_err_t link_err = link_tx_init();
    if (link_err == ESP_OK) {
        s_link_tx_ready = true;
        link_err = link_tx_send_rate(s_sample_rate);
    }
    if (link_err != ESP_OK) {
        ESP_LOGE(TAG, "UART link to ESP B unavailable: %s",
                 esp_err_to_name(link_err));
    }
    send_dsp_params_to_hub();
    i2s_init();
    xTaskCreatePinnedToCore(i2s_task, "i2s_task", 4096, NULL, 6,
                            &s_i2s_task_handle, 1);
    xTaskCreate(encoder_task, "encoder_task", 3072, NULL, 3,
                &s_encoder_task_handle);
    xTaskCreate(ui_task, "ui_task", 4096, NULL, 2, &s_ui_task_handle);
    bt_init();
}
