#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "protocol.h"

#include "crossover.h"

#include <string.h>

#define BOX_ROLE_GPIO GPIO_NUM_27
#define BOX_I2S_PORT I2S_NUM_0
#define BOX_I2S_BCLK_GPIO GPIO_NUM_18
#define BOX_I2S_WS_GPIO GPIO_NUM_19
#define BOX_I2S_DATA_GPIO GPIO_NUM_23
#define BOX_AUDIO_QUEUE_LENGTH 40U
#define BOX_AUDIO_TIMEOUT_MS 100
#define BOX_NO_AUDIO_TIMEOUT_US 1000000LL
#define BOX_WIFI_CONNECTED_BIT BIT0
#ifndef BOX_MONO_MODE
#define BOX_MONO_MODE 0
#endif
#define BOX_SERVO_INTERVAL_US 50000LL
#define BOX_LATE_DROP_US 6000LL

typedef struct {
    protocol_audio_header_t header;
    int16_t samples[PROTOCOL_CHUNK_FRAMES * 2U];
} box_audio_packet_t;

static const char *TAG = "ESP_BOX";
static EventGroupHandle_t s_wifi_events;
static QueueHandle_t s_audio_queue;
static i2s_chan_handle_t s_tx_channel;
static crossover_t s_crossover;
static uint8_t s_role;
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_registered;
static bool s_sync_valid;
static int64_t s_clock_offset_us;
static int64_t s_sync_request_t1;
static int64_t s_last_audio_us;
static uint32_t s_packets_lost;
static uint32_t s_queue_drops;
static int64_t s_playout_error_us;
static bool s_playing;

static bool station_connected(void)
{
    return (xEventGroupGetBits(s_wifi_events) & BOX_WIFI_CONNECTED_BIT) != 0;
}

static bool box_registered(void)
{
    bool registered;
    portENTER_CRITICAL(&s_status_mux);
    registered = s_registered;
    portEXIT_CRITICAL(&s_status_mux);
    return registered;
}

static void mark_unregistered(void)
{
    portENTER_CRITICAL(&s_status_mux);
    s_registered = false;
    s_sync_valid = false;
    portEXIT_CRITICAL(&s_status_mux);
}

static void set_playing(bool playing)
{
    portENTER_CRITICAL(&s_status_mux);
    s_playing = playing;
    portEXIT_CRITICAL(&s_status_mux);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, BOX_WIFI_CONNECTED_BIT);
        mark_unregistered();
        xQueueReset(s_audio_queue);
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting automatically");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, BOX_WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Connected to hub Wi-Fi");
    }
}

static void wifi_retry_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!station_connected()) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
                ESP_LOGW(TAG, "Wi-Fi reconnect request failed: %s",
                         esp_err_to_name(err));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

static esp_err_t wifi_start(void)
{
    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG,
                        "event loop init failed");
    if (esp_netif_create_default_wifi_sta() == NULL) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                wifi_event_handler, NULL));
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "Wi-Fi init failed");
    wifi_country_t country = {
        .cc = PROTOCOL_WIFI_COUNTRY,
        .schan = 1,
        .nchan = 13,
        .max_tx_power = 20,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_country(&country), TAG,
                        "country configuration failed");
    wifi_config_t config = {
        .sta = {
            .ssid = PROTOCOL_WIFI_SSID,
            .password = PROTOCOL_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                        "station mode setup failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG,
                        "station config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG,
                        "Wi-Fi power-save disable failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "station start failed");
    esp_err_t tx_power_err = esp_wifi_set_max_tx_power(78);
    if (tx_power_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi TX power setup failed after station start: %s; "
                 "using driver default", esp_err_to_name(tx_power_err));
    }
    if (xTaskCreate(wifi_retry_task, "box_wifi_retry", 3072, NULL, 3,
                    NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t i2s_start(uint32_t sample_rate)
{
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(BOX_I2S_PORT, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = PROTOCOL_CHUNK_FRAMES;
    channel_config.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &s_tx_channel, NULL),
                        TAG, "I2S channel create failed");
    i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOX_I2S_BCLK_GPIO,
            .ws = BOX_I2S_WS_GPIO,
            .dout = BOX_I2S_DATA_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_channel, &std_config),
                        TAG, "I2S standard mode init failed");
    return i2s_channel_enable(s_tx_channel);
}

static void control_task(void *arg)
{
    (void)arg;
    int socket_fd = -1;
    int64_t next_hello_us = 0;
    int64_t next_sync_us = 0;
    int64_t offset_samples[8] = {0};
    size_t offset_count = 0U;
    size_t offset_next = 0U;
    uint8_t buffer[64];
    struct sockaddr_in hub = {
        .sin_family = AF_INET,
        .sin_port = htons(PROTOCOL_CONTROL_PORT),
        .sin_addr.s_addr = inet_addr(PROTOCOL_AP_IPV4),
    };

    for (;;) {
        if (socket_fd < 0) {
            socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            if (socket_fd < 0) {
                ESP_LOGE(TAG, "Control socket create failed");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
            (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO,
                             &timeout, sizeof(timeout));
            struct sockaddr_in local = {
                .sin_family = AF_INET,
                .sin_port = htons(PROTOCOL_CONTROL_PORT),
                .sin_addr.s_addr = htonl(INADDR_ANY),
            };
            if (bind(socket_fd, (const struct sockaddr *)&local,
                     sizeof(local)) < 0) {
                ESP_LOGE(TAG, "Control socket bind failed");
                close(socket_fd);
                socket_fd = -1;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            ESP_LOGI(TAG, "Control UDP ready on port %u",
                     PROTOCOL_CONTROL_PORT);
        }

        int64_t now_us = esp_timer_get_time();
        bool connected = station_connected();
        bool registered = box_registered();
        if (connected && now_us >= next_hello_us) {
            protocol_hello_t hello = {
                .magic = PROTOCOL_MAGIC,
                .type = PROTOCOL_TYPE_HELLO,
                .role = s_role,
                .firmware_version = PROTOCOL_FIRMWARE_VERSION,
            };
            (void)sendto(socket_fd, &hello, sizeof(hello), 0,
                         (const struct sockaddr *)&hub, sizeof(hub));
            next_hello_us = now_us +
                (int64_t)(registered ? PROTOCOL_HEARTBEAT_MS : 1000U) * 1000;
        }
        if (connected && now_us >= next_sync_us) {
            protocol_sync_req_t request = {
                .magic = PROTOCOL_MAGIC,
                .type = PROTOCOL_TYPE_SYNC_REQ,
                .t1 = (uint64_t)now_us,
            };
            s_sync_request_t1 = now_us;
            (void)sendto(socket_fd, &request, sizeof(request), 0,
                         (const struct sockaddr *)&hub, sizeof(hub));
            next_sync_us = now_us + 1000000LL;
        }

        struct sockaddr_in source = {0};
        socklen_t source_length = sizeof(source);
        int received = recvfrom(socket_fd, buffer, sizeof(buffer), 0,
                                (struct sockaddr *)&source, &source_length);
        if (received == (int)sizeof(protocol_hello_ack_t)) {
            protocol_hello_ack_t ack;
            memcpy(&ack, buffer, sizeof(ack));
            if (ack.magic == PROTOCOL_MAGIC &&
                ack.type == PROTOCOL_TYPE_HELLO_ACK) {
                portENTER_CRITICAL(&s_status_mux);
                bool newly_registered = !s_registered;
                s_registered = true;
                portEXIT_CRITICAL(&s_status_mux);
                if (newly_registered) {
                    ESP_LOGI(TAG, "%s box registered with hub",
                             s_role == PROTOCOL_ROLE_LEFT ? "LEFT" : "RIGHT");
                }
            }
        } else if (received == (int)sizeof(protocol_sync_resp_t)) {
            protocol_sync_resp_t response;
            memcpy(&response, buffer, sizeof(response));
            int64_t t4 = esp_timer_get_time();
            if (response.magic == PROTOCOL_MAGIC &&
                response.type == PROTOCOL_TYPE_SYNC_RESP &&
                response.t1 == (uint64_t)s_sync_request_t1) {
                int64_t offset = ((int64_t)response.t1 -
                                  (int64_t)response.t2 +
                                  t4 - (int64_t)response.t3) / 2;
                if (offset_count >= 4U) {
                    int64_t sorted[8];
                    memcpy(sorted, offset_samples,
                           offset_count * sizeof(int64_t));
                    for (size_t i = 1; i < offset_count; i++) {
                        int64_t value = sorted[i];
                        size_t j = i;
                        while (j > 0U && sorted[j - 1U] > value) {
                            sorted[j] = sorted[j - 1U];
                            j--;
                        }
                        sorted[j] = value;
                    }
                    int64_t median = sorted[offset_count / 2U];
                    int64_t difference = offset - median;
                    if (difference < 0) difference = -difference;
                    if (difference > 5000) continue;
                }
                offset_samples[offset_next] = offset;
                offset_next = (offset_next + 1U) % 8U;
                if (offset_count < 8U) offset_count++;
                int64_t sorted[8];
                memcpy(sorted, offset_samples, offset_count * sizeof(int64_t));
                for (size_t i = 1; i < offset_count; i++) {
                    int64_t value = sorted[i];
                    size_t j = i;
                    while (j > 0U && sorted[j - 1U] > value) {
                        sorted[j] = sorted[j - 1U];
                        j--;
                    }
                    sorted[j] = value;
                }
                int64_t median = sorted[offset_count / 2U];
                portENTER_CRITICAL(&s_status_mux);
                s_clock_offset_us = median;
                s_sync_valid = true;
                portEXIT_CRITICAL(&s_status_mux);
            }
        }
    }
}

static void audio_rx_task(void *arg)
{
    (void)arg;
    uint8_t packet[PROTOCOL_AUDIO_PACKET_BYTES + 32U];
    uint32_t expected_sequence = 0U;
    bool have_sequence = false;
    int socket_fd = -1;
    for (;;) {
        if (socket_fd < 0) {
            socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            if (socket_fd < 0) {
                ESP_LOGE(TAG, "Audio socket create failed");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
            (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO,
                             &timeout, sizeof(timeout));
            struct sockaddr_in local = {
                .sin_family = AF_INET,
                .sin_port = htons(PROTOCOL_AUDIO_PORT),
                .sin_addr.s_addr = htonl(INADDR_ANY),
            };
            if (bind(socket_fd, (const struct sockaddr *)&local,
                     sizeof(local)) < 0) {
                ESP_LOGE(TAG, "Audio socket bind failed");
                close(socket_fd);
                socket_fd = -1;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            ESP_LOGI(TAG, "Audio UDP ready on port %u", PROTOCOL_AUDIO_PORT);
        }

        struct sockaddr_in source = {0};
        socklen_t source_length = sizeof(source);
        int received = recvfrom(socket_fd, packet, sizeof(packet), 0,
                                (struct sockaddr *)&source, &source_length);
        if (received != (int)PROTOCOL_AUDIO_PACKET_BYTES ||
            !box_registered()) {
            continue;
        }
        box_audio_packet_t audio = {0};
        memcpy(&audio.header, packet, sizeof(audio.header));
        if (audio.header.magic != PROTOCOL_MAGIC ||
            audio.header.type != PROTOCOL_TYPE_AUDIO ||
            audio.header.payload_len != PROTOCOL_AUDIO_PAYLOAD_BYTES ||
            (audio.header.sample_rate != 44100U &&
             audio.header.sample_rate != 48000U)) {
            continue;
        }
        memcpy(audio.samples, packet + sizeof(audio.header),
               PROTOCOL_AUDIO_PAYLOAD_BYTES);
        if (have_sequence && audio.header.seq > expected_sequence) {
            portENTER_CRITICAL(&s_status_mux);
            s_packets_lost += audio.header.seq - expected_sequence;
            portEXIT_CRITICAL(&s_status_mux);
        }
        expected_sequence = audio.header.seq + 1U;
        have_sequence = true;
        int64_t packet_time_us = esp_timer_get_time();
        portENTER_CRITICAL(&s_status_mux);
        s_last_audio_us = packet_time_us;
        portEXIT_CRITICAL(&s_status_mux);
        if (xQueueSend(s_audio_queue, &audio, 0) != pdTRUE) {
            box_audio_packet_t dropped;
            if (xQueueReceive(s_audio_queue, &dropped, 0) == pdTRUE) {
                portENTER_CRITICAL(&s_status_mux);
                s_queue_drops++;
                portEXIT_CRITICAL(&s_status_mux);
            }
            (void)xQueueSend(s_audio_queue, &audio, 0);
        }
    }
}

static void status_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        wifi_ap_record_t ap_info = {0};
        int rssi = station_connected() &&
                   esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK
            ? ap_info.rssi : 0;
        uint32_t lost;
        uint32_t drops;
        int64_t offset_us;
        int64_t error_us;
        bool sync_valid;
        bool playing;
        portENTER_CRITICAL(&s_status_mux);
        lost = s_packets_lost;
        drops = s_queue_drops;
        offset_us = s_clock_offset_us;
        error_us = s_playout_error_us;
        sync_valid = s_sync_valid;
        playing = s_playing;
        s_packets_lost = 0U;
        s_queue_drops = 0U;
        portEXIT_CRITICAL(&s_status_mux);
        ESP_LOGI(TAG, "role=%s wifi=%s registered=%u RSSI=%d dBm jitter=%u "
                 "packets lost=%lu queue drops=%lu offset=%lld us "
                 "playout error=%lld us playing=%u",
                 s_role == PROTOCOL_ROLE_LEFT ? "LEFT" : "RIGHT",
                 station_connected() ? "connected" : "disconnected",
                 box_registered() ? 1U : 0U, rssi,
                 (unsigned)uxQueueMessagesWaiting(s_audio_queue),
                 (unsigned long)lost, (unsigned long)drops,
                 (long long)(sync_valid ? offset_us : 0),
                 (long long)error_us, playing ? 1U : 0U);
    }
}

static void wait_until_us(int64_t deadline_us)
{
    for (;;) {
        int64_t remaining = deadline_us - esp_timer_get_time();
        if (remaining <= 250) return;
        if (remaining > 10000) {
            TickType_t ticks = pdMS_TO_TICKS((uint32_t)(remaining / 1000 - 2));
            if (ticks > 0U) {
                vTaskDelay(ticks);
                continue;
            }
        }
        esp_rom_delay_us((uint32_t)remaining);
    }
}

static esp_err_t update_i2s_rate(uint32_t sample_rate)
{
    esp_err_t err = i2s_channel_disable(s_tx_channel);
    if (err != ESP_OK) return err;
    i2s_std_clk_config_t clock_config = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    err = i2s_channel_reconfig_std_clock(s_tx_channel, &clock_config);
    if (err != ESP_OK) {
        (void)i2s_channel_enable(s_tx_channel);
        return err;
    }
    err = i2s_channel_enable(s_tx_channel);
    if (err != ESP_OK) return err;
    if (!crossover_init(&s_crossover, sample_rate)) {
        return ESP_ERR_INVALID_ARG;
    }
    crossover_set_muted(&s_crossover, true);
    return ESP_OK;
}

static void fade_to_mute(void)
{
    int16_t silence[PROTOCOL_CHUNK_FRAMES] = {0};
    int16_t output[PROTOCOL_CHUNK_FRAMES * 2U];
    crossover_set_muted(&s_crossover, true);
    for (unsigned chunk = 0; chunk < 3U; chunk++) {
        crossover_process(&s_crossover, silence, output,
                          PROTOCOL_CHUNK_FRAMES);
        size_t written = 0U;
        if (i2s_channel_write(s_tx_channel, output, sizeof(output), &written,
                              portMAX_DELAY) != ESP_OK) {
            break;
        }
    }
}

static void audio_player_task(void *arg)
{
    (void)arg;
    box_audio_packet_t packet;
    int16_t input[PROTOCOL_CHUNK_FRAMES + 1U];
    int16_t output[(PROTOCOL_CHUNK_FRAMES + 1U) * 2U];
    uint32_t current_rate = 44100U;
    int64_t last_correction_us = 0;
    bool playing = false;
    crossover_init(&s_crossover, current_rate);
    crossover_set_muted(&s_crossover, true);

    for (;;) {
        if (xQueueReceive(s_audio_queue, &packet,
                          pdMS_TO_TICKS(BOX_AUDIO_TIMEOUT_MS)) != pdTRUE) {
            int64_t last_audio;
            portENTER_CRITICAL(&s_status_mux);
            last_audio = s_last_audio_us;
            portEXIT_CRITICAL(&s_status_mux);
            if (last_audio == 0 ||
                esp_timer_get_time() - last_audio > BOX_NO_AUDIO_TIMEOUT_US ||
                !station_connected() || !box_registered()) {
                if (playing) fade_to_mute();
                else crossover_set_muted(&s_crossover, true);
                playing = false;
                set_playing(false);
            }
            continue;
        }
        if (!station_connected() || !box_registered()) {
            crossover_set_muted(&s_crossover, true);
            playing = false;
            set_playing(false);
            continue;
        }
        if (packet.header.sample_rate != current_rate ||
            (packet.header.flags & PROTOCOL_FLAG_STREAM_RESET) != 0U) {
            uint32_t new_rate = packet.header.sample_rate;
            if (new_rate != current_rate) {
                esp_err_t rate_err = update_i2s_rate(new_rate);
                if (rate_err != ESP_OK) {
                    ESP_LOGE(TAG, "I2S rate change to %lu Hz failed: %s",
                             (unsigned long)new_rate,
                             esp_err_to_name(rate_err));
                    continue;
                }
                current_rate = new_rate;
            } else if (!crossover_init(&s_crossover, current_rate)) {
                ESP_LOGE(TAG, "Crossover reset failed at %lu Hz",
                         (unsigned long)current_rate);
                continue;
            }
            playing = false;
            set_playing(false);
        }

        int64_t offset_us;
        bool sync_valid;
        portENTER_CRITICAL(&s_status_mux);
        offset_us = s_clock_offset_us;
        sync_valid = s_sync_valid;
        portEXIT_CRITICAL(&s_status_mux);
        int64_t deadline_us = (int64_t)packet.header.hub_time_us +
            (sync_valid ? offset_us : 0) + PROTOCOL_PLAYOUT_DELAY_US;
        int64_t now_us = esp_timer_get_time();
        if (now_us - deadline_us > BOX_LATE_DROP_US) {
            portENTER_CRITICAL(&s_status_mux);
            s_packets_lost++;
            portEXIT_CRITICAL(&s_status_mux);
            continue;
        }
        if (deadline_us > now_us) wait_until_us(deadline_us);
        int64_t playout_error_us = esp_timer_get_time() - deadline_us;
        portENTER_CRITICAL(&s_status_mux);
        s_playout_error_us = playout_error_us;
        portEXIT_CRITICAL(&s_status_mux);

        size_t input_frames = PROTOCOL_CHUNK_FRAMES;
        size_t channel_index = s_role == PROTOCOL_ROLE_LEFT ? 0U : 1U;
#if BOX_MONO_MODE
        for (size_t i = 0; i < input_frames; i++) {
            input[i] = (int16_t)(((int32_t)packet.samples[i * 2U] +
                                  packet.samples[i * 2U + 1U]) / 2);
        }
#else
        for (size_t i = 0; i < input_frames; i++) {
            input[i] = packet.samples[i * 2U + channel_index];
        }
#endif

        size_t queued = uxQueueMessagesWaiting(s_audio_queue);
        size_t target_queued = (size_t)(
            (uint64_t)PROTOCOL_PLAYOUT_DELAY_US * current_rate /
            1000000U / PROTOCOL_CHUNK_FRAMES);
        int64_t correction_now = esp_timer_get_time();
        bool insert_frame = false;
        bool drop_frame = false;
        if (correction_now - last_correction_us >= BOX_SERVO_INTERVAL_US) {
            if (queued > target_queued + 1U) {
                drop_frame = true;
            } else if (queued + 1U < target_queued) {
                insert_frame = true;
            }
            if (drop_frame || insert_frame) {
                last_correction_us = correction_now;
            }
        }
        if (insert_frame) {
            memmove(&input[1], &input[0],
                    PROTOCOL_CHUNK_FRAMES * sizeof(input[0]));
            input_frames++;
        } else if (drop_frame) {
            memmove(&input[0], &input[1],
                    (PROTOCOL_CHUNK_FRAMES - 1U) * sizeof(input[0]));
            input_frames--;
        }

        crossover_set_muted(&s_crossover, false);
        crossover_process(&s_crossover, input, output, input_frames);
        size_t written = 0U;
        esp_err_t write_err = i2s_channel_write(
            s_tx_channel, output, input_frames * 2U * sizeof(int16_t),
            &written, portMAX_DELAY);
        if (write_err != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(write_err));
            crossover_set_muted(&s_crossover, true);
        } else {
            playing = true;
            set_playing(true);
        }
    }
}

void app_main(void)
{
    const gpio_config_t role_gpio = {
        .pin_bit_mask = 1ULL << BOX_ROLE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&role_gpio));
    s_role = gpio_get_level(BOX_ROLE_GPIO) != 0
        ? PROTOCOL_ROLE_LEFT : PROTOCOL_ROLE_RIGHT;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    s_audio_queue = xQueueCreate(BOX_AUDIO_QUEUE_LENGTH,
                                 sizeof(box_audio_packet_t));
    if (s_audio_queue == NULL) {
        ESP_LOGE(TAG, "Audio jitter buffer allocation failed");
        return;
    }
    ESP_LOGI(TAG, "Speaker box ready, role=%s firmware=%s",
             s_role == PROTOCOL_ROLE_LEFT ? "LEFT" : "RIGHT",
             FIRMWARE_VERSION_NAME);
    ESP_ERROR_CHECK(wifi_start());
    ESP_ERROR_CHECK(i2s_start(44100U));
    if (xTaskCreate(control_task, "box_control", 4096, NULL, 4,
                    NULL) != pdPASS ||
        xTaskCreate(audio_rx_task, "box_audio_rx", 4096, NULL, 5,
                    NULL) != pdPASS ||
        xTaskCreate(status_task, "box_status", 3072, NULL, 2,
                    NULL) != pdPASS ||
        xTaskCreatePinnedToCore(audio_player_task, "box_audio_player",
                                8192, NULL, 8, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Could not create one or more audio tasks");
    }
}
