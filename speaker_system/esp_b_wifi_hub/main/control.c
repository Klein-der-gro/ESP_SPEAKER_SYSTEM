#include "hub.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"

#define HUB_CONTROL_RECV_TIMEOUT_MS 200

static const char *TAG = "HUB_CTRL";
static TaskHandle_t s_control_task;

static void handle_control_packet(int socket_fd, const uint8_t *buffer,
                                  size_t length,
                                  const struct sockaddr_in *source)
{
    if (length == sizeof(protocol_hello_t)) {
        protocol_hello_t hello;
        memcpy(&hello, buffer, sizeof(hello));
        if (hello.magic != PROTOCOL_MAGIC ||
            hello.type != PROTOCOL_TYPE_HELLO ||
            hello.role > PROTOCOL_ROLE_RIGHT) {
            return;
        }
        bool changed = hub_register_box(hello.role, source);
        protocol_hello_ack_t ack = {
            .magic = PROTOCOL_MAGIC,
            .type = PROTOCOL_TYPE_HELLO_ACK,
        };
        (void)sendto(socket_fd, &ack, sizeof(ack), 0,
                     (const struct sockaddr *)source, sizeof(*source));
        if (changed) {
            ESP_LOGI(TAG, "Registered %s box at %s",
                     hello.role == PROTOCOL_ROLE_LEFT ? "LEFT" : "RIGHT",
                     inet_ntoa(source->sin_addr));
        }
        return;
    }

    if (length == sizeof(protocol_sync_req_t)) {
        protocol_sync_req_t request;
        memcpy(&request, buffer, sizeof(request));
        if (request.magic != PROTOCOL_MAGIC ||
            request.type != PROTOCOL_TYPE_SYNC_REQ) {
            return;
        }
        protocol_sync_resp_t response = {
            .magic = PROTOCOL_MAGIC,
            .type = PROTOCOL_TYPE_SYNC_RESP,
            .t1 = request.t1,
            .t2 = (uint64_t)esp_timer_get_time(),
        };
        response.t3 = (uint64_t)esp_timer_get_time();
        (void)sendto(socket_fd, &response, sizeof(response), 0,
                     (const struct sockaddr *)source, sizeof(*source));
    }
}

static void control_task(void *arg)
{
    (void)arg;
    uint8_t buffer[64];
    int64_t last_status_us = 0;
    for (;;) {
        int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (socket_fd < 0) {
            ESP_LOGE(TAG, "Control socket create failed; retrying");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        struct timeval timeout = {
            .tv_sec = 0,
            .tv_usec = HUB_CONTROL_RECV_TIMEOUT_MS * 1000,
        };
        (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO,
                         &timeout, sizeof(timeout));
        struct sockaddr_in local = {
            .sin_family = AF_INET,
            .sin_port = htons(PROTOCOL_CONTROL_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(socket_fd, (const struct sockaddr *)&local,
                 sizeof(local)) < 0) {
            ESP_LOGE(TAG, "Control socket bind failed; retrying");
            close(socket_fd);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG, "Control UDP listening on port %u",
                 PROTOCOL_CONTROL_PORT);

        for (;;) {
            struct sockaddr_in source = {0};
            socklen_t source_length = sizeof(source);
            int received = recvfrom(socket_fd, buffer, sizeof(buffer), 0,
                                    (struct sockaddr *)&source,
                                    &source_length);
            if (received > 0) {
                handle_control_packet(socket_fd, buffer, (size_t)received,
                                      &source);
            }
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_status_us >= 1000000LL) {
                ESP_LOGI(TAG, "Registered boxes: %lu",
                         (unsigned long)hub_count_boxes(now_us));
                last_status_us = now_us;
            }
        }
    }
}

esp_err_t hub_control_start(void)
{
    if (xTaskCreate(control_task, "hub_control", 4096, NULL, 4,
                    &s_control_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
