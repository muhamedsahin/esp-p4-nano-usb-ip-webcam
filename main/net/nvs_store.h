#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Ev agi bilgisi NVS'te durur. Web formundan kaydedilen deger, menuconfig'i ezer. */

bool nvs_store_load_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
esp_err_t nvs_store_save_wifi(const char *ssid, const char *pass);
