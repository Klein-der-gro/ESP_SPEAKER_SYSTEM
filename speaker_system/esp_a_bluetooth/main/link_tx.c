#include "link_tx.h"

#include <stddef.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LINK_UART UART_NUM_2
#define LINK_UART_TX_GPIO 17
#define LINK_UART_BAUD 115200
#define LINK_UART_TX_BUFFER_SIZE 256
#define LINK_FRAME_TYPE_RATE 1
#define LINK_RATE_PAYLOAD_SIZE 4
#define LINK_FRAME_SIZE 8
#define LINK_HEARTBEAT_MS 1000

static const char *TAG = "LINK_TX";
static SemaphoreHandle_t s_link_mutex;
static uint32_t s_last_rate = 44100;

static uint8_t crc8(const uint8_t *data, size_t length)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80U) != 0U
                ? (uint8_t)((crc << 1) ^ 0x07U)
                : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t send_rate_locked(uint32_t sample_rate)
{
    uint8_t frame[LINK_FRAME_SIZE] = {
        0xA5,
        LINK_FRAME_TYPE_RATE,
        LINK_RATE_PAYLOAD_SIZE,
        (uint8_t)sample_rate,
        (uint8_t)(sample_rate >> 8),
        (uint8_t)(sample_rate >> 16),
        (uint8_t)(sample_rate >> 24),
        0
    };
    frame[LINK_FRAME_SIZE - 1] = crc8(&frame[1], LINK_FRAME_SIZE - 2);

    int written = uart_write_bytes(LINK_UART, (const char *)frame, sizeof(frame));
    if (written != (int)sizeof(frame)) {
        ESP_LOGE(TAG, "Sample-rate frame write failed (%d bytes)", written);
        return ESP_FAIL;
    }
    s_last_rate = sample_rate;
    return ESP_OK;
}

esp_err_t link_tx_send_rate(uint32_t sample_rate)
{
    if (s_link_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_link_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = send_rate_locked(sample_rate);
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
            xSemaphoreGive(s_link_mutex);
        }
    }
}

esp_err_t link_tx_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = LINK_UART_BAUD,
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

    s_link_mutex = xSemaphoreCreateMutex();
    if (s_link_mutex == NULL) {
        uart_driver_delete(LINK_UART);
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(link_heartbeat_task, "link_heartbeat", 2048, NULL, 1,
                    NULL) != pdPASS) {
        vSemaphoreDelete(s_link_mutex);
        s_link_mutex = NULL;
        uart_driver_delete(LINK_UART);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UART2 rate link ready: TX GPIO %d at %d baud",
             LINK_UART_TX_GPIO, LINK_UART_BAUD);
    return ESP_OK;
}
