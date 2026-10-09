/*
 * app_main.c
 *
 * Acilis sirasi:
 *   1. Hafiza
 *   2. Kare tamponu
 *   3. USB kamera (Wi-Fi beklerken kamera sayilabilsin)
 *   4. Ev agi ya da kurulum agi
 *   5. Web sunucusu
 */

#include "sdkconfig.h"

#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "config.h"
#include "frame_hub.h"
#include "http_server.h"
#include "usb_camera.h"
#include "wifi_manager.h"

static const char *TAG = "app";

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void log_banner(void)
{
    wifi_status_t wifi;
    wifi_manager_get_status(&wifi);

    uint8_t mac[6] = {0};
    wifi_interface_t iface = (wifi.mode == WIFI_LINK_AP) ? WIFI_IF_AP : WIFI_IF_STA;
    esp_wifi_get_mac(iface, mac);

    ESP_LOGI(TAG, "----------------------------------------");
    ESP_LOGI(TAG, "ESP32-P4 Nano USB webcam hazir");
    if (wifi.mode == WIFI_LINK_AP) {
        ESP_LOGI(TAG, "Kurulum agi : %s", wifi.ssid);
        if (strlen(CONFIG_WEBCAM_AP_PASSWORD) < 8) {
            ESP_LOGI(TAG, "Sifre        : (acik ag)");
        } else {
            ESP_LOGI(TAG, "Sifre        : %s", CONFIG_WEBCAM_AP_PASSWORD);
        }
    } else {
        ESP_LOGI(TAG, "Ev agi       : %s", wifi.ssid);
    }
    ESP_LOGI(TAG, "Site         : http://%s/", wifi.ip[0] ? wifi.ip : "0.0.0.0");
    if (wifi.mode == WIFI_LINK_STA) {
        ESP_LOGI(TAG, "Kisayol      : http://%s.local/", wifi.hostname);
    }
    ESP_LOGI(TAG, "MAC          : %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Kamera       : karttaki Type-A USB girisi");
    ESP_LOGI(TAG, "----------------------------------------");
}

void app_main(void)
{
    init_nvs();
    ESP_ERROR_CHECK(frame_hub_init());
    ESP_ERROR_CHECK(usb_camera_start());
    ESP_ERROR_CHECK(wifi_manager_start());
    ESP_ERROR_CHECK(http_server_start());
    log_banner();
}
