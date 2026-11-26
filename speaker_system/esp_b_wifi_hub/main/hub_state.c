#include "hub.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_timer.h"
#include "lwip/inet.h"

QueueHandle_t g_hub_audio_queue;
static portMUX_TYPE s_config_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_boxes_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_queue_overruns;
static uint32_t s_sample_rate = 44100;
static uint32_t s_pending_rate;
static bool s_rate_pending;
static protocol_dsp_params_t s_dsp_params;
static bool s_have_dsp_params;
static int64_t s_dsp_params_seen_us;
static hub_box_t s_boxes[HUB_BOX_COUNT];

void hub_state_init(void)
{
    g_hub_audio_queue = xQueueCreate(HUB_AUDIO_QUEUE_LENGTH,
                                     sizeof(hub_audio_chunk_t));
    if (g_hub_audio_queue == NULL) {
        return;
    }
    memset(&s_dsp_params, 0, sizeof(s_dsp_params));
    memset(s_boxes, 0, sizeof(s_boxes));
    s_queue_overruns = 0;
}

void hub_note_queue_overrun(void)
{
    portENTER_CRITICAL(&s_stats_mux);
    s_queue_overruns++;
    portEXIT_CRITICAL(&s_stats_mux);
}

uint32_t hub_get_queue_overruns(void)
{
    uint32_t count;
    portENTER_CRITICAL(&s_stats_mux);
    count = s_queue_overruns;
    portEXIT_CRITICAL(&s_stats_mux);
    return count;
}

void hub_set_sample_rate(uint32_t rate)
{
    if (rate != 44100U && rate != 48000U) {
        return;
    }
    portENTER_CRITICAL(&s_config_mux);
    if (rate != s_sample_rate) {
        s_pending_rate = rate;
        s_rate_pending = true;
    }
    portEXIT_CRITICAL(&s_config_mux);
}

bool hub_take_sample_rate(uint32_t *rate)
{
    if (rate == NULL) {
        return false;
    }
    bool pending;
    portENTER_CRITICAL(&s_config_mux);
    pending = s_rate_pending;
    if (pending) {
        *rate = s_pending_rate;
        s_rate_pending = false;
    }
    portEXIT_CRITICAL(&s_config_mux);
    return pending;
}

void hub_sample_rate_applied(uint32_t rate)
{
    portENTER_CRITICAL(&s_config_mux);
    s_sample_rate = rate;
    portEXIT_CRITICAL(&s_config_mux);
}

uint32_t hub_get_sample_rate(void)
{
    uint32_t rate;
    portENTER_CRITICAL(&s_config_mux);
    rate = s_sample_rate;
    portEXIT_CRITICAL(&s_config_mux);
    return rate;
}

void hub_set_dsp_params(const protocol_dsp_params_t *params)
{
    if (params == NULL) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_config_mux);
    s_dsp_params = *params;
    s_have_dsp_params = true;
    s_dsp_params_seen_us = now_us;
    portEXIT_CRITICAL(&s_config_mux);
}

bool hub_get_dsp_params(protocol_dsp_params_t *params, int64_t *last_seen_us)
{
    if (params == NULL || last_seen_us == NULL) {
        return false;
    }
    bool available;
    portENTER_CRITICAL(&s_config_mux);
    *params = s_dsp_params;
    *last_seen_us = s_dsp_params_seen_us;
    available = s_have_dsp_params;
    portEXIT_CRITICAL(&s_config_mux);
    return available;
}

bool hub_register_box(uint8_t role, const struct sockaddr_in *address)
{
    if (role > PROTOCOL_ROLE_RIGHT || address == NULL) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_boxes_mux);
    hub_box_t *box = &s_boxes[role];
    bool changed = !box->active ||
                   box->audio_addr.sin_addr.s_addr != address->sin_addr.s_addr;
    box->active = true;
    box->role = role;
    box->last_seen_us = now;
    box->audio_addr = *address;
    box->audio_addr.sin_port = htons(PROTOCOL_AUDIO_PORT);
    portEXIT_CRITICAL(&s_boxes_mux);
    return changed;
}

void hub_get_boxes(hub_box_t boxes[HUB_BOX_COUNT], int64_t now_us)
{
    if (boxes == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_boxes_mux);
    for (size_t i = 0; i < HUB_BOX_COUNT; i++) {
        if (s_boxes[i].active &&
            now_us - s_boxes[i].last_seen_us >
                (int64_t)PROTOCOL_BOX_TIMEOUT_MS * 1000) {
            s_boxes[i].active = false;
        }
        boxes[i] = s_boxes[i];
    }
    portEXIT_CRITICAL(&s_boxes_mux);
}

uint32_t hub_count_boxes(int64_t now_us)
{
    hub_box_t boxes[HUB_BOX_COUNT];
    uint32_t count = 0;
    hub_get_boxes(boxes, now_us);
    for (size_t i = 0; i < HUB_BOX_COUNT; i++) {
        if (boxes[i].active) {
            count++;
        }
    }
    return count;
}
