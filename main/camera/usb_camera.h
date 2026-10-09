#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Kamera cozunurlugu.
 * fps_x10: 300 = 30.0 kare/sn. Boylece metin yazarken kayan nokta printf gerekmez.
 */
typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t fps_x10;
} cam_mode_t;

typedef struct {
    bool connected;
    bool streaming;
    uint16_t width;
    uint16_t height;
    uint16_t fps_x10;
    uint32_t frames;
    uint32_t overflows;
    char error[16];
} cam_status_t;

esp_err_t usb_camera_start(void);
void usb_camera_get_status(cam_status_t *out);
int usb_camera_get_modes(cam_mode_t *out, int max);

/* Listede olan bir MJPEG moduna gecer. Akis acik degilse ESP_ERR_INVALID_STATE. */
esp_err_t usb_camera_set_mode(uint16_t width, uint16_t height, uint16_t fps_x10);
