/*
 * usb_camera.c
 *
 * Type-A girisindeki UVC kamerayi acar, MJPEG kareleri frame_hub'a koyar.
 * Cozunurluk degisince surucu yayini durdurup yeni modda yeniden baslatir.
 * Kimse izlemiyorsa USB yayini bir sure sonra durur; sayfa acilinca geri gelir.
 */

#include "usb_camera.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

#include "config.h"
#include "frame_hub.h"
#include "text_util.h"

static const char *TAG = "cam";

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_device_sem;
static SemaphoreHandle_t s_fmt_lock;
static SemaphoreHandle_t s_fmt_done;
static QueueHandle_t s_frames;
static QueueHandle_t s_fmt_q;

static uvc_host_stream_hdl_t s_stream;
static volatile bool s_have_stream;
static volatile bool s_stream_on;
static volatile bool s_accept_frames;
static volatile bool s_device_here;
static volatile bool s_disconnect;
static volatile uint32_t s_overflows;
static uint8_t s_dev_addr;
static int s_open_fails;
static esp_err_t s_fmt_result;

static cam_mode_t s_modes[CAM_MAX_MODES];
static int s_mode_count;
static cam_status_t s_status;

static float interval_to_fps(uint32_t interval)
{
    if (interval == 0) {
        return 0.0f;
    }
    return 10000000.0f / (float)interval;
}

static void set_error(const char *code)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy_cstr(s_status.error, sizeof(s_status.error), code);
    xSemaphoreGive(s_lock);
}

static int mode_score(const cam_mode_t *mode)
{
    static const int targets[][2] = {
        {1280, 720},
        {640, 480},
        {800, 600},
        {320, 240},
        {1920, 1080},
    };
    int best = 1000000;
    for (int i = 0; i < 5; i++) {
        int distance = abs((int)mode->width - targets[i][0]) + abs((int)mode->height - targets[i][1]);
        int score = distance + i * 80;
        if (score < best) {
            best = score;
        }
    }
    /* 15 fps civari, ev Wi-Fi'sinde takilmadan izlemek icin baslangic tercihi. */
    return best * 10 + abs((int)mode->fps_x10 - 150);
}

static int best_index(void)
{
    int best = 0;
    int best_score = 0x7fffffff;
    for (int i = 0; i < s_mode_count; i++) {
        int score = mode_score(&s_modes[i]);
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

static int smallest_index(void)
{
    int best = 0;
    uint32_t best_area = 0xffffffffu;
    for (int i = 0; i < s_mode_count; i++) {
        uint32_t area = (uint32_t)s_modes[i].width * s_modes[i].height;
        if (area < best_area) {
            best_area = area;
            best = i;
        }
    }
    return best;
}

static void sort_modes(cam_mode_t *modes, int count)
{
    for (int i = 1; i < count; i++) {
        cam_mode_t key = modes[i];
        int j = i - 1;
        while (j >= 0) {
            bool greater = modes[j].width > key.width ||
                           (modes[j].width == key.width && modes[j].height > key.height) ||
                           (modes[j].width == key.width && modes[j].height == key.height &&
                            modes[j].fps_x10 > key.fps_x10);
            if (!greater) {
                break;
            }
            modes[j + 1] = modes[j];
            j--;
        }
        modes[j + 1] = key;
    }
}

static void load_modes(uint8_t addr)
{
    uvc_host_frame_info_t raw[CAM_MAX_MODES];
    size_t count = CAM_MAX_MODES;
    esp_err_t err = uvc_host_get_frame_list(addr, 0, (uvc_host_frame_info_t (*)[])raw, &count);

    cam_mode_t parsed[CAM_MAX_MODES];
    int n = 0;
    if (err == ESP_OK) {
        for (size_t i = 0; i < count && n < CAM_MAX_MODES; i++) {
            if (raw[i].format != UVC_VS_FORMAT_MJPEG || raw[i].h_res == 0 || raw[i].v_res == 0) {
                continue;
            }
            uint16_t fps = fps_to_x10(interval_to_fps(raw[i].default_interval));
            if (fps == 0) {
                fps = 150;
            }
            bool duplicate = false;
            for (int j = 0; j < n; j++) {
                if (parsed[j].width == raw[i].h_res && parsed[j].height == raw[i].v_res &&
                    parsed[j].fps_x10 == fps) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            parsed[n].width = (uint16_t)raw[i].h_res;
            parsed[n].height = (uint16_t)raw[i].v_res;
            parsed[n].fps_x10 = fps;
            n++;
        }
    }
    sort_modes(parsed, n);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_mode_count = n;
    if (n > 0) {
        memcpy(s_modes, parsed, sizeof(cam_mode_t) * (size_t)n);
    }
    s_status.connected = true;
    s_status.error[0] = '\0';
    if (n == 0) {
        copy_cstr(s_status.error, sizeof(s_status.error), "mjpeg_yok");
    }
    xSemaphoreGive(s_lock);

    if (n == 0) {
        ESP_LOGW(TAG, "Kamerada MJPEG modu yok");
        return;
    }
    for (int i = 0; i < n; i++) {
        char fps[8];
        fps_x10_to_text(fps, sizeof(fps), parsed[i].fps_x10);
        ESP_LOGI(TAG, "MJPEG %u x %u @ %s fps", parsed[i].width, parsed[i].height, fps);
    }
}

static bool on_frame(const uvc_host_frame_t *frame, void *user_ctx)
{
    (void)user_ctx;
    if (!s_accept_frames || !frame) {
        return true;
    }
    if (xQueueSend(s_frames, &frame, 0) != pdPASS) {
        return true;
    }
    return false;
}

static void on_stream_event(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        ESP_LOGW(TAG, "USB aktarim hatasi: %s", esp_err_to_name(event->transfer_error.error));
        break;
    case UVC_HOST_DEVICE_DISCONNECTED:
        s_device_here = false;
        s_disconnect = true;
        ESP_LOGW(TAG, "Kamera cikarildi");
        break;
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        s_overflows++;
        break;
#ifdef UVC_HOST_SUSPEND_RESUME_API_SUPPORTED
    case UVC_HOST_DEVICE_SUSPENDED:
    case UVC_HOST_DEVICE_RESUMED:
        break;
#endif
    default:
        break;
    }
}

static void on_driver_event(const uvc_host_driver_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event->type != UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        return;
    }
    if (event->device_connected.uvc_stream_index != 0) {
        return;
    }

    s_dev_addr = event->device_connected.dev_addr;
    ESP_LOGI(TAG, "Kamera gorundu, adres %u", s_dev_addr);
    load_modes(s_dev_addr);
    s_disconnect = false;
    s_device_here = true;
    xSemaphoreGive(s_device_sem);
}

static void usb_lib_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void return_queued_frames(void)
{
    uvc_host_frame_t *frame = NULL;
    while (xQueueReceive(s_frames, &frame, 0) == pdPASS) {
        if (s_stream && frame) {
            uvc_host_frame_return(s_stream, frame);
        }
    }
}

static void note_frame(const uvc_host_frame_t *frame)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.connected = true;
    s_status.streaming = true;
    s_status.width = (uint16_t)frame->vs_format.h_res;
    s_status.height = (uint16_t)frame->vs_format.v_res;
    s_status.fps_x10 = fps_to_x10(frame->vs_format.fps);
    s_status.frames++;
    s_status.overflows = s_overflows;
    s_status.error[0] = '\0';
    xSemaphoreGive(s_lock);
}

static void fail_pending_format(esp_err_t err)
{
    cam_mode_t ignored;
    if (xQueueReceive(s_fmt_q, &ignored, 0) == pdPASS) {
        s_fmt_result = err;
        xSemaphoreGive(s_fmt_done);
    }
}

static void apply_format(const cam_mode_t *mode)
{
    s_accept_frames = false;
    vTaskDelay(pdMS_TO_TICKS(20));
    return_queued_frames();

    uvc_host_stream_format_t fmt = {
        .h_res = mode->width,
        .v_res = mode->height,
        .fps = mode->fps_x10 / 10.0f,
        .format = UVC_VS_FORMAT_MJPEG,
    };
    bool was_on = s_stream_on;
    esp_err_t err = uvc_host_stream_format_select(s_stream, &fmt);
    if (err == ESP_OK) {
        s_stream_on = was_on;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.width = (uint16_t)fmt.h_res;
        s_status.height = (uint16_t)fmt.v_res;
        s_status.fps_x10 = fps_to_x10(fmt.fps);
        xSemaphoreGive(s_lock);
        char text[8];
        fps_x10_to_text(text, sizeof(text), fps_to_x10(fmt.fps));
        ESP_LOGI(TAG, "Mod %u x %u @ %s fps", (unsigned)fmt.h_res, (unsigned)fmt.v_res, text);
    } else {
        ESP_LOGW(TAG, "Mod degismedi: %s", esp_err_to_name(err));
    }
    s_accept_frames = true;
    s_fmt_result = err;
    xSemaphoreGive(s_fmt_done);
}

static void update_streaming_flag(bool on)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.streaming = on;
    s_status.overflows = s_overflows;
    xSemaphoreGive(s_lock);
}

static void service_viewers(int64_t *idle_from)
{
    int viewers = frame_hub_viewers();
    int64_t now = esp_timer_get_time();
    static int64_t s_retry_start_us;

    if (viewers > 0) {
        *idle_from = 0;
        if (!s_stream_on && s_stream) {
            if (now < s_retry_start_us) {
                return;
            }
            s_accept_frames = true;
            esp_err_t err = uvc_host_stream_start(s_stream);
            if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
                s_stream_on = true;
                s_retry_start_us = 0;
                ESP_LOGI(TAG, "Izleyici geldi, USB yayin acildi");
            } else {
                s_retry_start_us = now + 1000000;
            }
        }
        return;
    }

    if (!s_stream_on) {
        *idle_from = 0;
        return;
    }
    if (*idle_from == 0) {
        *idle_from = now;
        return;
    }
    if (now - *idle_from < CAM_IDLE_STOP_US) {
        return;
    }

    s_accept_frames = false;
    vTaskDelay(pdMS_TO_TICKS(20));
    return_queued_frames();
    if (s_stream && uvc_host_stream_stop(s_stream) == ESP_OK) {
        s_stream_on = false;
        update_streaming_flag(false);
        ESP_LOGI(TAG, "Izleyici yok, USB yayin durdu");
    } else {
        s_accept_frames = true;
    }
    *idle_from = 0;
}

static void streaming_loop(void)
{
    int64_t idle_from = 0;
    int64_t last_log = 0;

    while (s_device_here && !s_disconnect && s_stream) {
        uvc_host_frame_t *frame = NULL;
        if (xQueueReceive(s_frames, &frame, pdMS_TO_TICKS(200)) == pdPASS) {
            uvc_host_frame_t *newer = NULL;
            while (xQueueReceive(s_frames, &newer, 0) == pdPASS) {
                uvc_host_frame_return(s_stream, frame);
                frame = newer;
            }
            if (frame->data && frame->data_len > 0) {
                if (frame_hub_publish(frame->data, frame->data_len)) {
                    note_frame(frame);
                }
            }
            uvc_host_frame_return(s_stream, frame);
        }

        if (!s_device_here || s_disconnect || !s_stream) {
            break;
        }

        cam_mode_t request;
        if (xQueueReceive(s_fmt_q, &request, 0) == pdPASS) {
            apply_format(&request);
        }
        service_viewers(&idle_from);

        int64_t now = esp_timer_get_time();
        if (s_stream_on && now - last_log > 5000000) {
            cam_status_t status;
            usb_camera_get_status(&status);
            char text[8];
            fps_x10_to_text(text, sizeof(text), status.fps_x10);
            ESP_LOGI(TAG, "Akiyor %u x %u @ %s fps, kare %u",
                     status.width, status.height, text, (unsigned)status.frames);
            last_log = now;
        }
    }
    fail_pending_format(ESP_ERR_INVALID_STATE);
}

static esp_err_t open_stream(const cam_mode_t *mode)
{
    uvc_host_stream_config_t cfg = {
        .event_cb = on_stream_event,
        .frame_cb = on_frame,
        .user_ctx = NULL,
        .usb = {
            .dev_addr = s_dev_addr,
            .vid = UVC_HOST_ANY_VID,
            .pid = UVC_HOST_ANY_PID,
            .uvc_stream_index = 0,
        },
        .vs_format = {
            .h_res = mode->width,
            .v_res = mode->height,
            .fps = mode->fps_x10 / 10.0f,
            .format = UVC_VS_FORMAT_MJPEG,
        },
        .advanced = {
            .number_of_frame_buffers = CAM_USB_BUFFERS,
            /* Surucu tamponu PSRAM'de. 1 MB, 1080p MJPEG'i kesmeden alir. */
            .frame_size = 1024 * 1024,
            .frame_heap_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
            /* URB'ler dahili RAM'de kaldigi icin kucuk tutulur. */
            .number_of_urbs = 4,
            .urb_size = 10 * 1024,
            .user_frame_buffers = NULL,
        },
    };

    char text[8];
    fps_x10_to_text(text, sizeof(text), mode->fps_x10);
    ESP_LOGI(TAG, "Kamera aciliyor %u x %u @ %s fps", mode->width, mode->height, text);

    s_accept_frames = true;
    uvc_host_stream_hdl_t stream = NULL;
    esp_err_t err = uvc_host_stream_open(&cfg, pdMS_TO_TICKS(5000), &stream);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Kamera acilamadi: %s", esp_err_to_name(err));
        return err;
    }

    s_stream = stream;
    s_have_stream = true;
    err = uvc_host_stream_start(stream);
    s_stream_on = (err == ESP_OK);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Yayin baslamadi: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

static void close_stream(void)
{
    s_accept_frames = false;
    vTaskDelay(pdMS_TO_TICKS(20));
    return_queued_frames();
    if (s_stream) {
        uvc_host_stream_close(s_stream);
        s_stream = NULL;
    }
    s_stream_on = false;
    s_have_stream = false;
}

static void mark_disconnected(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.connected = false;
    s_status.streaming = false;
    s_status.width = 0;
    s_status.height = 0;
    s_status.fps_x10 = 0;
    s_status.error[0] = '\0';
    s_mode_count = 0;
    xSemaphoreGive(s_lock);
}

static bool take_open_mode(cam_mode_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_mode_count > 0;
    if (ok) {
        int index = (s_open_fails == 0) ? best_index() : smallest_index();
        *out = s_modes[index];
    }
    xSemaphoreGive(s_lock);
    return ok;
}

static void camera_task(void *arg)
{
    (void)arg;
    while (true) {
        ESP_LOGI(TAG, "USB kamera bekleniyor (Type-A)");
        xSemaphoreTake(s_device_sem, portMAX_DELAY);
        if (!s_device_here) {
            continue;
        }

        s_open_fails = 0;
        if (s_mode_count == 0) {
            while (s_device_here && !s_disconnect) {
                vTaskDelay(pdMS_TO_TICKS(400));
            }
            mark_disconnected();
            continue;
        }

        while (s_device_here && !s_disconnect) {
            cam_mode_t mode;
            if (!take_open_mode(&mode)) {
                break;
            }
            if (s_open_fails > 0) {
                ESP_LOGW(TAG, "Buyuk mod acilmadi, en kucuk MJPEG deneniyor");
            }
            if (open_stream(&mode) == ESP_OK) {
                streaming_loop();
                close_stream();
                break;
            }
            s_open_fails++;
            if (s_open_fails >= 3) {
                set_error("acilamadi");
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        mark_disconnected();
    }
}

void usb_camera_get_status(cam_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.overflows = s_overflows;
    *out = s_status;
    xSemaphoreGive(s_lock);
}

int usb_camera_get_modes(cam_mode_t *out, int max)
{
    if (!out || max <= 0) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int count = s_mode_count;
    if (count > max) {
        count = max;
    }
    for (int i = 0; i < count; i++) {
        out[i] = s_modes[i];
    }
    xSemaphoreGive(s_lock);
    return count;
}

esp_err_t usb_camera_set_mode(uint16_t width, uint16_t height, uint16_t fps_x10)
{
    if (!s_have_stream) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t target = fps_x10 ? fps_x10 : 150;
    cam_mode_t chosen = {0};
    bool found = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int best_gap = 100000;
    for (int i = 0; i < s_mode_count; i++) {
        if (s_modes[i].width != width || s_modes[i].height != height) {
            continue;
        }
        int gap = abs((int)s_modes[i].fps_x10 - (int)target);
        if (gap < best_gap) {
            best_gap = gap;
            chosen = s_modes[i];
            found = true;
        }
    }
    xSemaphoreGive(s_lock);

    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }
    if (xSemaphoreTake(s_fmt_lock, pdMS_TO_TICKS(3500)) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(s_fmt_done, 0);
    if (xQueueOverwrite(s_fmt_q, &chosen) != pdPASS) {
        xSemaphoreGive(s_fmt_lock);
        return ESP_FAIL;
    }
    if (xSemaphoreTake(s_fmt_done, pdMS_TO_TICKS(3000)) != pdPASS) {
        xSemaphoreGive(s_fmt_lock);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = s_fmt_result;
    xSemaphoreGive(s_fmt_lock);
    return err;
}

esp_err_t usb_camera_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_fmt_lock = xSemaphoreCreateMutex();
    s_device_sem = xSemaphoreCreateBinary();
    s_fmt_done = xSemaphoreCreateBinary();
    s_frames = xQueueCreate(CAM_USB_BUFFERS, sizeof(uvc_host_frame_t *));
    s_fmt_q = xQueueCreate(1, sizeof(cam_mode_t));
    if (!s_lock || !s_fmt_lock || !s_device_sem || !s_fmt_done || !s_frames || !s_fmt_q) {
        return ESP_ERR_NO_MEM;
    }

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB Host kurulamadi: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL, 17, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    const uvc_host_driver_config_t driver = {
        .driver_task_stack_size = 6 * 1024,
        .driver_task_priority = 18,
        .xCoreID = 1,
        .create_background_task = true,
        .event_cb = on_driver_event,
        .user_ctx = NULL,
    };
    err = uvc_host_install(&driver);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UVC surucusu kurulamadi: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreatePinnedToCore(camera_task, "camera", 8192, NULL, 14, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
