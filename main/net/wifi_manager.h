#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    WIFI_LINK_OFF = 0,
    WIFI_LINK_STA,
    WIFI_LINK_AP,
} wifi_link_mode_t;

typedef struct {
    wifi_link_mode_t mode;
    bool connected;
    char ssid[33];
    char ip[16];
    char hostname[32];
    int8_t rssi;
} wifi_status_t;

#define WIFI_SCAN_MAX 12

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} wifi_scan_item_t;

/* Ilk baglanti bitene kadar bekler. Ev agi olmazsa kurulum agini acar. */
esp_err_t wifi_manager_start(void);
void wifi_manager_get_status(wifi_status_t *out);
esp_err_t wifi_manager_scan(wifi_scan_item_t *out, int *count);
