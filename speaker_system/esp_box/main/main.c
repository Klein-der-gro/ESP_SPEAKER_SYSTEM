#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

#define BOX_ROLE_GPIO GPIO_NUM_27

static const char *TAG = "ESP_BOX";

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

    int role_level = gpio_get_level(BOX_ROLE_GPIO);
    ESP_LOGI(TAG, "ESP box ready (milestone M0), role: %s",
             role_level != 0 ? "LEFT" : "RIGHT");
}
