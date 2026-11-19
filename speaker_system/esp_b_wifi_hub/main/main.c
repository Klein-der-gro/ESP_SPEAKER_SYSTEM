#include "esp_log.h"
#include "protocol.h"

static const char *TAG = "WIFI_HUB";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP B Wi-Fi hub ready (milestone M0), firmware: %s",
             FIRMWARE_VERSION_NAME);
}
