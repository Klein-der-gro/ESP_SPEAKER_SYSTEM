#include "hub.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "HUB_WIFI";
static esp_netif_t *s_ap_netif;

esp_err_t hub_wifi_start(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG,
                        "event loop init failed");
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_netif_ip_info_t ip_info = {0};
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_stop(s_ap_netif), TAG,
                        "DHCP stop failed");
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_ap_netif, &ip_info), TAG,
                        "AP address setup failed");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(s_ap_netif), TAG,
                        "DHCP start failed");

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "Wi-Fi init failed");
    wifi_country_t country = {
        .cc = PROTOCOL_WIFI_COUNTRY,
        .schan = 1,
        .nchan = 13,
        .max_tx_power = 20,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_country(&country), TAG,
                        "country configuration failed");
    wifi_config_t ap_config = {
        .ap = {
            .ssid = PROTOCOL_WIFI_SSID,
            .password = PROTOCOL_WIFI_PASSWORD,
            .channel = PROTOCOL_WIFI_CHANNEL,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = HUB_BOX_COUNT,
            .beacon_interval = 100,
        },
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG,
                        "AP mode setup failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG,
                        "AP config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG,
                        "Wi-Fi power-save disable failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_max_tx_power(78), TAG,
                        "Wi-Fi TX power setup failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "AP start failed");

    ESP_LOGI(TAG, "AP up: SSID=%s channel=%u IP=192.168.4.1 country=%s",
             PROTOCOL_WIFI_SSID, PROTOCOL_WIFI_CHANNEL,
             PROTOCOL_WIFI_COUNTRY);
    return ESP_OK;
}
