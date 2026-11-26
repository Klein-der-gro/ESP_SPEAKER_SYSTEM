#include "link_tx.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LINK_UART UART_NUM_2
#define LINK_UART_TX_GPIO 17
#define LINK_UART_TX_BUFFER_SIZE 256
#define LINK_HEARTBEAT_MS 1000
#define LINK_PARAMS_MIN_INTERVAL_MS 50

static const char *TAG = "LINK_TX";
static SemaphoreHandle_t s_link_mutex;
static uint32_t s_last_rate = 44100;
static protocol_dsp_params_t s_last_params;
static bool s_have_params;
static TickType_t s_last_params_sent;
static bool s_link_ready;

static esp_err_t send_frame_locked(uint8_t type, const void *payload,
                                   size_t payload_size)
{
    uint8_t frame[3U + PROTOCOL_UART_DSP_PARAMS_PAYLOAD_SIZE + 1U];
    if (payload_size > PROTOCOL_UART_DSP_PARAMS_PAYLOAD_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    frame[0] = PROTOCOL_UART_START_BYTE;
    frame[1] = type;
    frame[2] = (uint8_t)payload_size;
    memcpy(&frame[3], payload, payload_size);
    frame[3U + payload_size] = protocol_crc8(&frame[1], 2U + payload_size);

    int written = uart_write_bytes(LINK_UART, (const char *)frame,
                                   4U + payload_size);
    if (written != (int)(4U + payload_size)) {
        ESP_LOGE(TAG, "UART frame write failed (%d bytes)", written);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t send_rate_locked(uint32_t sample_rate)
{
    const uint8_t payload[PROTOCOL_UART_RATE_PAYLOAD_SIZE] = {
        (uint8_t)sample_rate,
        (uint8_t)(sample_rate >> 8),
        (uint8_t)(sample_rate >> 16),
        (uint8_t)(sample_rate >> 24)
    };
    esp_err_t err = send_frame_locked(PROTOCOL_UART_TYPE_RATE, payload,
                                      sizeof(payload));
    if (err != ESP_OK) return err;
    s_last_rate = sample_rate;
    return ESP_OK;
}

esp_err_t link_tx_send_rate(uint32_t sample_rate)
{
    if (s_link_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_link_ready) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_link_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = send_rate_locked(sample_rate);
    xSemaphoreGive(s_link_mutex);
    return err;
}

esp_err_t link_tx_send_dsp_params(const protocol_dsp_params_t *params)
{
    if (params == NULL || s_link_mutex == NULL || !s_link_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_link_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    bool changed = !s_have_params ||
                   memcmp(&s_last_params, params, sizeof(*params)) != 0;
    s_last_params = *params;
    s_have_params = true;
    TickType_t now = xTaskGetTickCount();
    TickType_t min_interval = pdMS_TO_TICKS(LINK_PARAMS_MIN_INTERVAL_MS);
    esp_err_t err = ESP_OK;
    if (changed && (now - s_last_params_sent >= min_interval)) {
        err = send_frame_locked(PROTOCOL_UART_TYPE_DSP_PARAMS, params,
                                sizeof(*params));
        if (err == ESP_OK) s_last_params_sent = now;
    }
    xSemaphoreGive(s_link_mutex);
    return err;
}

static void link_heartbeat_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LINK_HEARTBEAT_MS));
        if (xSemaphoreTake(s_link_mutex, portMAX_DELAY) == pdTRUE) {
            (void)send_rate_locked(s_last_rate);
            if (s_have_params) {
                if (send_frame_locked(PROTOCOL_UART_TYPE_DSP_PARAMS,
                                      &s_last_params,
                                      sizeof(s_last_params)) == ESP_OK) {
                    s_last_params_sent = xTaskGetTickCount();
                }
            }
            xSemaphoreGive(s_link_mutex);
        }
    }
}

esp_err_t link_tx_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = PROTOCOL_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(LINK_UART, &uart_config);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(LINK_UART, LINK_UART_TX_GPIO, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_driver_install(LINK_UART, LINK_UART_TX_BUFFER_SIZE,
                              LINK_UART_TX_BUFFER_SIZE, 0,
                              NULL, 0);
    if (err != ESP_OK) {
        return err;
    }

    s_link_ready = true;
    s_link_mutex = xSemaphoreCreateMutex();
    if (s_link_mutex == NULL) {
        s_link_ready = false;
        uart_driver_delete(LINK_UART);
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(link_heartbeat_task, "link_heartbeat", 2048, NULL, 1,
                    NULL) != pdPASS) {
        vSemaphoreDelete(s_link_mutex);
        s_link_mutex = NULL;
        s_link_ready = false;
        uart_driver_delete(LINK_UART);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UART2 rate link ready: TX GPIO %d at %d baud",
             LINK_UART_TX_GPIO, PROTOCOL_UART_BAUD);
    return ESP_OK;
}
