#pragma once

/*
 * Ince ayar tek yerde durur.
 * Kare boyutu 32'nin kati olmali; PSRAM hizali ayirma bunu ister.
 */

#define FRAME_SLOT_COUNT 3
#define FRAME_SLOT_BYTES (768 * 1024)
#define FRAME_MAX_VIEWERS 3

#define CAM_MAX_MODES 24
#define CAM_USB_BUFFERS 3

#define WIFI_STA_RETRIES 8
#define WIFI_AP_SSID "P4-Webcam"
#define WIFI_AP_CHANNEL 6
#define WIFI_AP_MAX_CLIENTS 4

/* Izleyici gidince USB yayini bu sure sonunda durur (mikrosaniye). */
#define CAM_IDLE_STOP_US (5LL * 1000 * 1000)
