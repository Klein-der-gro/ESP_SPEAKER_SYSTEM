#include "hub.h"

#include <math.h>
#include <string.h>

#include "driver/uart.h"
#include "dsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HUB_UART UART_NUM_2
#define HUB_UART_TX_GPIO 4
#define HUB_UART_RX_GPIO 16
#define HUB_UART_RX_BUFFER_SIZE 512
#define HUB_UART_TX_BUFFER_SIZE 256
#define UART_MAX_PAYLOAD PROTOCOL_UART_DSP_PARAMS_PAYLOAD_SIZE

static const char *TAG = "HUB_LINK";
static TaskHandle_t s_link_task;

esp_err_t hub_link_send_box_status(uint8_t status)
{
    uint8_t frame[4U + PROTOCOL_UART_BOX_STATUS_PAYLOAD_SIZE];
    frame[0] = PROTOCOL_UART_START_BYTE;
    frame[1] = PROTOCOL_UART_TYPE_BOX_STATUS;
    frame[2] = PROTOCOL_UART_BOX_STATUS_PAYLOAD_SIZE;
    frame[3] = status;
    frame[4] = protocol_crc8(&frame[1], 2U +
                             PROTOCOL_UART_BOX_STATUS_PAYLOAD_SIZE);
    int written = uart_write_bytes(HUB_UART, (const char *)frame,
                                   sizeof(frame));
    if (written != sizeof(frame)) {
        ESP_LOGE(TAG, "Box status UART write failed (%d bytes)", written);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool valid_params(const protocol_dsp_params_t *params)
{
    return isfinite(params->bass_db) && isfinite(params->treble_db) &&
           isfinite(params->vocal_db) &&
           params->bass_db >= -15.0f && params->bass_db <= 60.0f &&
           params->treble_db >= -10.0f && params->treble_db <= 45.0f &&
           params->vocal_db >= -10.0f && params->vocal_db <= 20.0f &&
           params->balance >= -100 && params->balance <= 100 &&
           params->volume >= 0 && params->volume <= DSP_VOLUME_MAX &&
           params->muted <= 1U && params->vocal_mode <= 2U;
}

static void handle_frame(const uint8_t *frame, uint8_t payload_size)
{
    if (frame[0] == PROTOCOL_UART_TYPE_RATE &&
        payload_size == PROTOCOL_UART_RATE_PAYLOAD_SIZE) {
        uint32_t rate = (uint32_t)frame[2] |
                        ((uint32_t)frame[3] << 8) |
                        ((uint32_t)frame[4] << 16) |
                        ((uint32_t)frame[5] << 24);
        if (rate == 44100U || rate == 48000U) {
            hub_set_sample_rate(rate);
        }
    } else if (frame[0] == PROTOCOL_UART_TYPE_DSP_PARAMS &&
               payload_size == PROTOCOL_UART_DSP_PARAMS_PAYLOAD_SIZE) {
        protocol_dsp_params_t params;
        memcpy(&params, &frame[2], sizeof(params));
        if (valid_params(&params)) {
            hub_set_dsp_params(&params);
        }
    }
}

static void parse_byte(uint8_t byte, uint8_t frame[2U + UART_MAX_PAYLOAD + 1U],
                       size_t *used, size_t *expected)
{
    if (*used == 0U) {
        if (byte == PROTOCOL_UART_START_BYTE) {
            *used = 1U;
        }
        return;
    }
    if (*used == 1U) {
        frame[0] = byte;
        *used = 2U;
        return;
    }
    if (*used == 2U) {
        if (byte > UART_MAX_PAYLOAD) {
            *used = 0U;
            *expected = 0U;
            return;
        }
        frame[1] = byte;
        *expected = 4U + (size_t)byte;
        *used = 3U;
        return;
    }

    frame[*used - 1U] = byte;
    (*used)++;
    if (*used == *expected) {
        uint8_t payload_size = frame[1];
        uint8_t calculated_crc = protocol_crc8(frame, 2U + payload_size);
        uint8_t received_crc = frame[2U + payload_size];
        if (calculated_crc == received_crc) {
            handle_frame(frame, payload_size);
        }
        *used = 0U;
        *expected = 0U;
    }
}

static void link_rx_task(void *arg)
{
    (void)arg;
    uint8_t bytes[64];
    uint8_t frame[2U + UART_MAX_PAYLOAD + 1U] = {0};
    size_t used = 0U;
    size_t expected = 0U;
    for (;;) {
        int count = uart_read_bytes(HUB_UART, bytes, sizeof(bytes),
                                    pdMS_TO_TICKS(100));
        for (int i = 0; i < count; i++) {
            parse_byte(bytes[i], frame, &used, &expected);
        }
    }
}

esp_err_t hub_link_rx_start(void)
{
    const uart_config_t config = {
        .baud_rate = PROTOCOL_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_param_config(HUB_UART, &config), TAG,
                        "UART configuration failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(HUB_UART, HUB_UART_TX_GPIO,
                                     HUB_UART_RX_GPIO, UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE), TAG,
                        "UART TX/RX pin setup failed");
    ESP_RETURN_ON_ERROR(uart_driver_install(HUB_UART,
                                            HUB_UART_RX_BUFFER_SIZE,
                                            HUB_UART_TX_BUFFER_SIZE,
                                            0, NULL, 0), TAG,
                        "UART driver install failed");
    if (xTaskCreate(link_rx_task, "hub_link_rx", 3072, NULL, 5,
                    &s_link_task) != pdPASS) {
        uart_driver_delete(HUB_UART);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UART2 full-duplex ready: TX GPIO %d RX GPIO %d at %d baud",
             HUB_UART_TX_GPIO, HUB_UART_RX_GPIO, PROTOCOL_UART_BAUD);
    return ESP_OK;
}
