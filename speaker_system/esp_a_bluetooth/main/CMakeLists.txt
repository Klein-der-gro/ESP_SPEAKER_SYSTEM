idf_component_register(SRCS "main.c" "oled.c" "link_tx.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash esp_driver_i2s esp_driver_gpio
                                esp_driver_i2c esp_driver_uart esp_timer protocol dsp)
