/*
 * frame_hub.c
 *
 * USB'den gelen JPEG burada en fazla uc slota yazilir.
 * Izleyici yavas kalsa bile eski kare kuyrugu buyumez; herkes en yeni kareyi alir.
 * Slot, okunurken uzerine yazilmaz.
 */

#include "frame_hub.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "config.h"

static const char *TAG = "hub";

typedef struct {
    uint8_t *data;
    size_t cap;
    size_t len;
    uint32_t seq;
    int readers;
} frame_slot_t;

static frame_slot_t s_slots[FRAME_SLOT_COUNT];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_published = -1;
static uint32_t s_seq;
static uint32_t s_published_total;
static uint32_t s_dropped;
static TaskHandle_t s_waiters[FRAME_MAX_VIEWERS];

static int64_t s_window_start;
static uint32_t s_window_frames;
static uint32_t s_window_bytes;
static uint16_t s_fps_x10;
static uint32_t s_kbps;
static int64_t s_last_publish;
static size_t s_cap;
static bool s_logged_big;

static const uint8_t *find_jpeg(const uint8_t *data, size_t len, size_t *out_len)
{
    size_t scan = len < 64 ? len : 64;
    for (size_t i = 0; i + 1 < scan; i++) {
        if (data[i] == 0xFF && data[i + 1] == 0xD8) {
            *out_len = len - i;
            return data + i;
        }
    }
    return NULL;
}

static void update_rate(size_t len)
{
    int64_t now = esp_timer_get_time();
    s_last_publish = now;
    if (s_window_start == 0) {
        s_window_start = now;
    }
    s_window_frames++;
    s_window_bytes += (uint32_t)len;

    int64_t dt = now - s_window_start;
    if (dt < 1000000) {
        return;
    }
    s_fps_x10 = (uint16_t)((s_window_frames * 10000000LL) / dt);
    s_kbps = (uint32_t)(((uint64_t)s_window_bytes * 8000ULL) / (uint64_t)dt);
    s_window_frames = 0;
    s_window_bytes = 0;
    s_window_start = now;
}

esp_err_t frame_hub_init(void)
{
    static const size_t tries[] = {FRAME_SLOT_BYTES, 384 * 1024, 192 * 1024};
    size_t cap = 0;
    uint8_t *mem[FRAME_SLOT_COUNT] = {0};

    for (size_t t = 0; t < sizeof(tries) / sizeof(tries[0]) && cap == 0; t++) {
        bool ok = true;
        for (int i = 0; i < FRAME_SLOT_COUNT; i++) {
            mem[i] = heap_caps_aligned_alloc(32, tries[t], MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!mem[i]) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            for (int i = 0; i < FRAME_SLOT_COUNT; i++) {
                if (mem[i]) {
                    heap_caps_free(mem[i]);
                    mem[i] = NULL;
                }
            }
            continue;
        }
        cap = tries[t];
    }

    if (cap == 0) {
        ESP_LOGE(TAG, "PSRAM kare tamponu ayrilamadi");
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < FRAME_SLOT_COUNT; i++) {
        s_slots[i].data = mem[i];
        s_slots[i].cap = cap;
    }
    s_cap = cap;
    ESP_LOGI(TAG, "Kare tamponu %u KB x %d", (unsigned)(cap / 1024), FRAME_SLOT_COUNT);
    return ESP_OK;
}

bool frame_hub_publish(const uint8_t *data, size_t len)
{
    size_t jpeg_len = 0;
    const uint8_t *jpeg = find_jpeg(data, len, &jpeg_len);
    if (!jpeg || jpeg_len < 32) {
        s_dropped++;
        return false;
    }
    if (jpeg_len > s_cap) {
        if (!s_logged_big) {
            ESP_LOGW(TAG, "Kare %u bayt, tampon %u. Daha dusuk cozunurluk sec.",
                     (unsigned)jpeg_len, (unsigned)s_cap);
            s_logged_big = true;
        }
        s_dropped++;
        return false;
    }

    int slot = -1;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < FRAME_SLOT_COUNT; i++) {
        if (i != s_published && s_slots[i].readers == 0) {
            slot = i;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_mux);

    if (slot < 0) {
        s_dropped++;
        return false;
    }

    memcpy(s_slots[slot].data, jpeg, jpeg_len);

    TaskHandle_t wake[FRAME_MAX_VIEWERS];
    int wake_count = 0;

    taskENTER_CRITICAL(&s_mux);
    s_slots[slot].len = jpeg_len;
    s_slots[slot].seq = ++s_seq;
    s_published = slot;
    s_published_total++;
    for (int i = 0; i < FRAME_MAX_VIEWERS; i++) {
        if (s_waiters[i]) {
            wake[wake_count++] = s_waiters[i];
        }
    }
    taskEXIT_CRITICAL(&s_mux);

    update_rate(jpeg_len);
    for (int i = 0; i < wake_count; i++) {
        xTaskNotifyGive(wake[i]);
    }
    return true;
}

static bool try_acquire(frame_view_t *view, uint32_t newer_than)
{
    bool found = false;
    taskENTER_CRITICAL(&s_mux);
    int slot = s_published;
    if (slot >= 0 && s_slots[slot].len > 0 && s_slots[slot].seq > newer_than) {
        s_slots[slot].readers++;
        view->data = s_slots[slot].data;
        view->len = s_slots[slot].len;
        view->seq = s_slots[slot].seq;
        view->slot = slot;
        found = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    return found;
}

bool frame_hub_acquire(frame_view_t *view, uint32_t newer_than, uint32_t wait_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t budget = pdMS_TO_TICKS(wait_ms);
    for (;;) {
        if (try_acquire(view, newer_than)) {
            return true;
        }
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= budget) {
            return false;
        }
        ulTaskNotifyTake(pdTRUE, budget - elapsed);
    }
}

void frame_hub_release(const frame_view_t *view)
{
    if (!view || view->slot < 0 || view->slot >= FRAME_SLOT_COUNT) {
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    if (s_slots[view->slot].readers > 0) {
        s_slots[view->slot].readers--;
    }
    taskEXIT_CRITICAL(&s_mux);
}

int frame_hub_watch(void *task)
{
    int id = -1;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < FRAME_MAX_VIEWERS; i++) {
        if (s_waiters[i] == NULL) {
            s_waiters[i] = (TaskHandle_t)task;
            id = i;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return id;
}

void frame_hub_unwatch(int id)
{
    if (id < 0 || id >= FRAME_MAX_VIEWERS) {
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    s_waiters[id] = NULL;
    taskEXIT_CRITICAL(&s_mux);
}

int frame_hub_viewers(void)
{
    int count = 0;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < FRAME_MAX_VIEWERS; i++) {
        if (s_waiters[i]) {
            count++;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return count;
}

void frame_hub_get_stats(frame_stats_t *out)
{
    int64_t now = esp_timer_get_time();
    bool fresh = s_last_publish != 0 && (now - s_last_publish) < 1500000;

    taskENTER_CRITICAL(&s_mux);
    out->published = s_published_total;
    out->dropped = s_dropped;
    out->fps_x10 = fresh ? s_fps_x10 : 0;
    out->kbps = fresh ? s_kbps : 0;
    out->viewers = 0;
    for (int i = 0; i < FRAME_MAX_VIEWERS; i++) {
        if (s_waiters[i]) {
            out->viewers++;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
}
