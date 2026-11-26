idf_component_register(SRCS "main.c" "crossover.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_driver_i2s esp_wifi
                                esp_event esp_netif esp_timer nvs_flash lwip
                                protocol)
