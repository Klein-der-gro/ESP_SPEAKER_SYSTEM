#ifndef ESP_B_WIFI_HUB_H
#define ESP_B_WIFI_HUB_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "protocol.h"

#define HUB_CHUNK_FRAMES PROTOCOL_CHUNK_FRAMES
#define HUB_AUDIO_QUEUE_LENGTH 4
#define HUB_BOX_COUNT 2

typedef struct {
    uint64_t hub_time_us;
    uint64_t frame_index;
    uint32_t sample_rate;
    uint8_t flags;
    int16_t samples[HUB_CHUNK_FRAMES * 2U];
} hub_audio_chunk_t;

typedef struct {
    bool active;
    uint8_t role;
    int64_t last_seen_us;
    struct sockaddr_in audio_addr;
} hub_box_t;

extern QueueHandle_t g_hub_audio_queue;
void hub_state_init(void);
void hub_note_queue_overrun(void);
uint32_t hub_get_queue_overruns(void);
void hub_set_sample_rate(uint32_t rate);
bool hub_take_sample_rate(uint32_t *rate);
void hub_sample_rate_applied(uint32_t rate);
uint32_t hub_get_sample_rate(void);
void hub_set_dsp_params(const protocol_dsp_params_t *params);
bool hub_get_dsp_params(protocol_dsp_params_t *params, int64_t *last_seen_us);
bool hub_register_box(uint8_t role, const struct sockaddr_in *address);
void hub_get_boxes(hub_box_t boxes[HUB_BOX_COUNT], int64_t now_us);
uint32_t hub_count_boxes(int64_t now_us);

esp_err_t hub_wifi_start(void);
esp_err_t hub_link_rx_start(void);
esp_err_t hub_i2s_rx_start(void);
esp_err_t hub_control_start(void);
esp_err_t hub_udp_audio_start(void);

#endif
