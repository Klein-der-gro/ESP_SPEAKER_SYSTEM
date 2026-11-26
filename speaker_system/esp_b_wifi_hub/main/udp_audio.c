#include "hub.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "HUB_AUDIO";
static TaskHandle_t s_audio_task;

static void audio_task(void *arg)
{
    (void)arg;
    hub_audio_chunk_t chunk;
    uint8_t packet[PROTOCOL_AUDIO_PACKET_BYTES];
    uint32_t sequence = 0U;
    uint64_t packets_sent = 0U;
    int64_t last_status_us = esp_timer_get_time();
    int socket_fd = -1;

    for (;;) {
        if (socket_fd < 0) {
            socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            if (socket_fd < 0) {
                ESP_LOGE(TAG, "UDP audio socket create failed; retrying");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            ESP_LOGI(TAG, "UDP audio sender ready on port %u",
                     PROTOCOL_AUDIO_PORT);
        }
        if (xQueueReceive(g_hub_audio_queue, &chunk,
                          pdMS_TO_TICKS(100)) == pdTRUE) {
            protocol_audio_header_t header = {
                .magic = PROTOCOL_MAGIC,
                .type = PROTOCOL_TYPE_AUDIO,
                .flags = (uint8_t)(chunk.flags |
                    (chunk.frame_index == 0U ? PROTOCOL_FLAG_STREAM_RESET : 0U)),
                .payload_len = PROTOCOL_AUDIO_PAYLOAD_BYTES,
                .seq = sequence++,
                .sample_rate = chunk.sample_rate,
                .frame_index = chunk.frame_index,
                .hub_time_us = chunk.hub_time_us,
            };
            memcpy(packet, &header, sizeof(header));
            memcpy(packet + sizeof(header), chunk.samples,
                   PROTOCOL_AUDIO_PAYLOAD_BYTES);

            hub_box_t boxes[HUB_BOX_COUNT];
            int64_t now_us = esp_timer_get_time();
            hub_get_boxes(boxes, now_us);
            for (size_t i = 0; i < HUB_BOX_COUNT; i++) {
                if (!boxes[i].active) continue;
                int sent = sendto(socket_fd, packet, sizeof(packet), 0,
                                  (const struct sockaddr *)&boxes[i].audio_addr,
                                  sizeof(boxes[i].audio_addr));
                if (sent == (int)sizeof(packet)) {
                    packets_sent++;
                } else {
                    ESP_LOGW(TAG, "Audio packet send failed for role %u",
                             boxes[i].role);
                }
            }
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us - last_status_us >= 1000000LL) {
            ESP_LOGI(TAG, "boxes=%lu packets/s=%llu queue_overruns=%lu rate=%lu",
                     (unsigned long)hub_count_boxes(now_us),
                     (unsigned long long)packets_sent,
                     (unsigned long)hub_get_queue_overruns(),
                     (unsigned long)hub_get_sample_rate());
            packets_sent = 0U;
            last_status_us = now_us;
        }
    }
}

esp_err_t hub_udp_audio_start(void)
{
    if (xTaskCreate(audio_task, "hub_udp_audio", 4096, NULL, 5,
                    &s_audio_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
