idf_component_register(SRCS "main.c" "udp_audio.c" "control.c" "i2s_rx.c" "link_rx.c" "wifi_ap.c" "hub_state.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_event esp_netif esp_wifi esp_driver_i2s
                                esp_driver_uart esp_timer nvs_flash lwip
                                protocol dsp)
