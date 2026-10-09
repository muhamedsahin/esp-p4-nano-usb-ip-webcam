#include "nvs_store.h"

#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "nvs";
static const char *NS = "webcam";

bool nvs_store_load_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t handle;
    if (nvs_open(NS, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t needed = ssid_len;
    esp_err_t err = nvs_get_str(handle, "ssid", ssid, &needed);
    if (err != ESP_OK || ssid[0] == '\0') {
        nvs_close(handle);
        return false;
    }

    needed = pass_len;
    err = nvs_get_str(handle, "pass", pass, &needed);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        pass[0] = '\0';
        err = ESP_OK;
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Kayitli sifre okunamadi");
        return false;
    }
    return true;
}

esp_err_t nvs_store_save_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "pass", pass ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
