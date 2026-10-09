#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    const uint8_t *data;
    size_t len;
    uint32_t seq;
    int slot;
} frame_view_t;

typedef struct {
    uint32_t published;
    uint32_t dropped;
    uint16_t fps_x10;
    uint32_t kbps;
    int viewers;
} frame_stats_t;

esp_err_t frame_hub_init(void);

/* JPEG baytlarini en yeni kare olarak yayinlar. Eski karelerin sirasini tutmaz. */
bool frame_hub_publish(const uint8_t *data, size_t len);

/* newer_than degerinden yeni bir kare varsa onu kilitler. Bitince release sart. */
bool frame_hub_acquire(frame_view_t *view, uint32_t newer_than, uint32_t wait_ms);
void frame_hub_release(const frame_view_t *view);

/* Izleyici gorevi, yeni kare gelince uyansin diye kendini yazdirir. Doluyorsa -1. */
int frame_hub_watch(void *task);
void frame_hub_unwatch(int id);
int frame_hub_viewers(void);
void frame_hub_get_stats(frame_stats_t *out);
