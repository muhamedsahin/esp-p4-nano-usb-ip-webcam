/*
 * wifi_manager.c
 *
 * Once kayitli veya menuconfig'teki ev agina baglanir.
 * Birkaç denemeden sonra olmazsa telefonun gorecegi P4-Webcam agini acar.
 * Ag bir kez tutulduysa kopunca yeniden dener, kurulum agina geri donmez.
 */

#include "wifi_manager.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "sdkconfig.h"

#include "config.h"
#include "nvs_store.h"
#include "text_util.h"

static const char *TAG = "wifi";

#define WIFI_OK_BIT (1u << 0)
#define WIFI_FAIL_BIT (1u << 1)

static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_lock;
static wifi_status_t s_status;
static int s_retries;
static volatile bool s_sta_active;
static bool s_ever_connected;

/* strncpy, tam dolu SSID/sifrede -Werror=stringop-truncation veriyor. */
static void copy_wifi_field(uint8_t *dst, size_t dst_len, const char *src)
{
    size_t i = 0;
    if (dst_len == 0) {
        return;
    }
    if (src) {
        while (src[i] && i + 1 < dst_len) {
            dst[i] = (uint8_t)src[i];
            i++;
        }
    }
    while (i < dst_len) {
        dst[i++] = 0;
    }
}

static void fill_ip(char *dst, size_t dst_len, const esp_ip4_addr_t *ip)
{
    snprintf(dst, dst_len, IPSTR, IP2STR(ip));
}

static void remember(wifi_link_mode_t mode, bool connected, const char *ssid, const char *ip)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.mode = mode;
    s_status.connected = connected;
    if (ssid) {
        copy_cstr(s_status.ssid, sizeof(s_status.ssid), ssid);
    }
    if (ip) {
        copy_cstr(s_status.ip, sizeof(s_status.ip), ip);
    }
    xSemaphoreGive(s_lock);
}

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS acilamadi, IP ile girilebilir");
        return;
    }
    mdns_hostname_set(CONFIG_WEBCAM_HOSTNAME);
    mdns_instance_name_set("ESP32-P4 USB Webcam");
    mdns_service_add(NULL, "_http", "_tcp", CONFIG_WEBCAM_HTTP_PORT, NULL, 0);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (!s_sta_active) {
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        remember(WIFI_LINK_STA, false, NULL, NULL);
        if (s_ever_connected) {
            ESP_LOGW(TAG, "Ev agi koptu, yeniden deneniyor");
            esp_wifi_connect();
            return;
        }
        if (s_retries >= WIFI_STA_RETRIES) {
            xEventGroupSetBits(s_events, WIFI_FAIL_BIT);
            return;
        }
        s_retries++;
        ESP_LOGW(TAG, "Ev agina baglanilamadi, deneme %d", s_retries);
        esp_wifi_connect();
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        char ip[16];
        fill_ip(ip, sizeof(ip), &event->ip_info.ip);
        s_retries = 0;
        s_ever_connected = true;
        remember(WIFI_LINK_STA, true, NULL, ip);
        ESP_LOGI(TAG, "Ev agi tamam, IP %s", ip);
        xEventGroupSetBits(s_events, WIFI_OK_BIT);
    }
}

static esp_err_t start_station(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    copy_wifi_field(cfg.sta.ssid, sizeof(cfg.sta.ssid), ssid);
    copy_wifi_field(cfg.sta.password, sizeof(cfg.sta.password), pass);
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    if (pass[0] == '\0') {
        cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else {
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    }

    remember(WIFI_LINK_STA, false, ssid, "");
    s_sta_active = true;
    s_retries = 0;
    xEventGroupClearBits(s_events, WIFI_OK_BIT | WIFI_FAIL_BIT);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Uyku, video karelerinin arasina bosluk sokar. */
    esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi uyku kapatilamadi: %s", esp_err_to_name(ps));
    }

    ESP_LOGI(TAG, "Ev agi araniyor: %s", ssid);
    EventBits_t bits = xEventGroupWaitBits(
        s_events, WIFI_OK_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_OK_BIT) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_set_hostname(netif, CONFIG_WEBCAM_HOSTNAME);
        }
        start_mdns();
        return ESP_OK;
    }

    s_sta_active = false;
    return ESP_FAIL;
}

static esp_err_t start_setup_ap(bool wifi_was_started)
{
    s_sta_active = false;
    if (wifi_was_started) {
        esp_wifi_stop();
    }

    wifi_config_t ap = {0};
    copy_wifi_field(ap.ap.ssid, sizeof(ap.ap.ssid), WIFI_AP_SSID);
    ap.ap.ssid_len = strlen(WIFI_AP_SSID);
    ap.ap.channel = WIFI_AP_CHANNEL;
    ap.ap.max_connection = WIFI_AP_MAX_CLIENTS;

    if (strlen(CONFIG_WEBCAM_AP_PASSWORD) < 8) {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        copy_wifi_field(ap.ap.password, sizeof(ap.ap.password), CONFIG_WEBCAM_AP_PASSWORD);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    char ip[16] = "192.168.4.1";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t info;
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        fill_ip(ip, sizeof(ip), &info.ip);
    }
    remember(WIFI_LINK_AP, true, WIFI_AP_SSID, ip);
    ESP_LOGW(TAG, "Kurulum agi acildi: %s  http://%s", WIFI_AP_SSID, ip);
    return ESP_OK;
}

static void load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    if (nvs_store_load_wifi(ssid, ssid_len, pass, pass_len)) {
        ESP_LOGI(TAG, "Wi-Fi bilgisi kartin hafizasindan okundu");
        return;
    }
    copy_cstr(ssid, ssid_len, CONFIG_WEBCAM_WIFI_SSID);
    copy_cstr(pass, pass_len, CONFIG_WEBCAM_WIFI_PASSWORD);
}

esp_err_t wifi_manager_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    if (!s_lock || !s_events) {
        return ESP_ERR_NO_MEM;
    }

    copy_cstr(s_status.hostname, sizeof(s_status.hostname), CONFIG_WEBCAM_HOSTNAME);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    char ssid[33] = {0};
    char pass[65] = {0};
    load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));

    if (ssid[0] == '\0') {
        return start_setup_ap(false);
    }
    if (start_station(ssid, pass) == ESP_OK) {
        return ESP_OK;
    }
    return start_setup_ap(true);
}

void wifi_manager_get_status(wifi_status_t *out)
{
    int8_t rssi = 0;
    bool have_rssi = false;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
        have_rssi = true;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    if (have_rssi && out->mode == WIFI_LINK_STA && out->connected) {
        out->rssi = rssi;
    }
    xSemaphoreGive(s_lock);
}

esp_err_t wifi_manager_scan(wifi_scan_item_t *out, int *count)
{
    if (!out || !count || *count <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_scan_config_t scan = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t found = 16;
    wifi_ap_record_t *recs = calloc(found, sizeof(*recs));
    if (!recs) {
        return ESP_ERR_NO_MEM;
    }
    err = esp_wifi_scan_get_ap_records(&found, recs);
    if (err != ESP_OK) {
        free(recs);
        return err;
    }

    int kept = 0;
    int limit = *count;
    for (int i = 0; i < (int)found && kept < limit; i++) {
        if (recs[i].ssid[0] == '\0') {
            continue;
        }
        int slot = -1;
        for (int j = 0; j < kept; j++) {
            if (strcmp(out[j].ssid, (const char *)recs[i].ssid) == 0) {
                slot = j;
                break;
            }
        }
        if (slot >= 0) {
            if (recs[i].rssi > out[slot].rssi) {
                out[slot].rssi = recs[i].rssi;
                out[slot].secure = recs[i].authmode != WIFI_AUTH_OPEN;
            }
            continue;
        }
        copy_cstr(out[kept].ssid, sizeof(out[kept].ssid), (const char *)recs[i].ssid);
        out[kept].rssi = recs[i].rssi;
        out[kept].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        kept++;
    }

    for (int i = 1; i < kept; i++) {
        wifi_scan_item_t key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].rssi < key.rssi) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }

    *count = kept;
    free(recs);
    return ESP_OK;
}
