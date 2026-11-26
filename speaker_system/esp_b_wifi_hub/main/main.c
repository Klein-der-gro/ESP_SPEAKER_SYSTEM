#include "hub.h"

#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "WIFI_HUB";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    hub_state_init();
    if (g_hub_audio_queue == NULL) {
        ESP_LOGE(TAG, "Audio queue allocation failed");
        return;
    }

    ESP_ERROR_CHECK(hub_wifi_start());
    ESP_ERROR_CHECK(hub_link_rx_start());
    ESP_ERROR_CHECK(hub_i2s_rx_start());
    ESP_ERROR_CHECK(hub_control_start());
    ESP_ERROR_CHECK(hub_udp_audio_start());
    ESP_LOGI(TAG, "ESP B hub ready, firmware: %s", FIRMWARE_VERSION_NAME);
}
