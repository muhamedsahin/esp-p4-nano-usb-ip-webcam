/*
 * http_server.c
 *
 * Sayfa, stil ve betik karta gomuludur. Ayrica bir bilgisayar gerekmez.
 * Video ayri bir gorevde gider; bu sayede cozunurluk istegi yayini kilitlemez.
 */

#include "http_server.h"

#include <errno.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#include "config.h"
#include "frame_hub.h"
#include "http_api.h"

static const char *TAG = "http";

/* IDF 6 sembol adi yalniz dosya adindan gelir: web/index.html -> index_html */
extern const uint8_t web_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t web_index_html_end[] asm("_binary_index_html_end");
extern const uint8_t web_app_css_start[] asm("_binary_app_css_start");
extern const uint8_t web_app_css_end[] asm("_binary_app_css_end");
extern const uint8_t web_app_js_start[] asm("_binary_app_js_start");
extern const uint8_t web_app_js_end[] asm("_binary_app_js_end");

typedef struct {
    const uint8_t *start;
    const uint8_t *end;
    const char *type;
} web_blob_t;

static const web_blob_t s_page = {
    web_index_html_start, web_index_html_end, "text/html; charset=utf-8",
};
static const web_blob_t s_css = {
    web_app_css_start, web_app_css_end, "text/css; charset=utf-8",
};
static const web_blob_t s_js = {
    web_app_js_start, web_app_js_end, "text/javascript; charset=utf-8",
};

static esp_err_t send_blob(httpd_req_t *req)
{
    const web_blob_t *blob = (const web_blob_t *)req->user_ctx;
    httpd_resp_set_type(req, blob->type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    size_t len = (size_t)(blob->end - blob->start);
    return httpd_resp_send(req, (const char *)blob->start, len);
}

static esp_err_t favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static bool send_part(httpd_req_t *req, const frame_view_t *view)
{
    char hdr[96];
    int n = snprintf(hdr, sizeof(hdr),
                     "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                     (unsigned)view->len);
    if (n <= 0 || httpd_resp_send_chunk(req, hdr, n) != ESP_OK) {
        return false;
    }
    return httpd_resp_send_chunk(req, (const char *)view->data, view->len) == ESP_OK;
}

/* IDF hata listesinde 503 yok; durum satirini kendimiz yaziyoruz. */
static void send_too_many(httpd_req_t *req)
{
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_send(req, "Cok fazla izleyici", HTTPD_RESP_USE_STRLEN);
}

/* Kare gelmese bile tarayici sekmesi kapaninca gorev birikmesin. */
static bool client_gone(int sock)
{
    char tmp;
    int n = recv(sock, &tmp, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0) {
        return true;
    }
    return n < 0 && errno != EAGAIN && errno != EWOULDBLOCK;
}

static void stream_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    int watch = frame_hub_watch(xTaskGetCurrentTaskHandle());
    if (watch < 0) {
        send_too_many(req);
        httpd_req_async_handler_complete(req);
        vTaskDelete(NULL);
        return;
    }

    int sock = httpd_req_to_sockfd(req);
    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    uint32_t seen = 0;
    while (true) {
        frame_view_t view;
        if (!frame_hub_acquire(&view, seen, 1000)) {
            if (client_gone(sock)) {
                break;
            }
            continue;
        }
        seen = view.seq;
        bool ok = send_part(req, &view);
        frame_hub_release(&view);
        if (!ok) {
            break;
        }
    }

    frame_hub_unwatch(watch);
    httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

static esp_err_t stream_get(httpd_req_t *req)
{
    if (frame_hub_viewers() >= FRAME_MAX_VIEWERS) {
        send_too_many(req);
        return ESP_FAIL;
    }

    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        return ESP_FAIL;
    }
    if (xTaskCreatePinnedToCore(stream_task, "stream", 6144, copy, 5, NULL, 0) != pdPASS) {
        httpd_resp_send_err(copy, HTTPD_500_INTERNAL_SERVER_ERROR, "Gorev acilamadi");
        httpd_req_async_handler_complete(copy);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t snapshot_get(httpd_req_t *req)
{
    frame_view_t view;
    if (!frame_hub_acquire(&view, 0, 800) || view.len == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Hazir kare yok");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, (const char *)view.data, view.len);
    frame_hub_release(&view);
    return err;
}

static void route(httpd_handle_t server, const char *uri, httpd_method_t method,
                  esp_err_t (*handler)(httpd_req_t *), void *ctx)
{
    httpd_uri_t item = {
        .uri = uri,
        .method = method,
        .handler = handler,
        .user_ctx = ctx,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &item));
}

esp_err_t http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_WEBCAM_HTTP_PORT;
    config.max_open_sockets = 7;
    config.max_uri_handlers = 12;
    /* Eski soketi silmek, acik video yayinini da koparabiliyor. */
    config.lru_purge_enable = false;
    config.stack_size = 8192;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 8;
    config.core_id = 0;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Sunucu acilamadi: %s", esp_err_to_name(err));
        return err;
    }

    route(server, "/", HTTP_GET, send_blob, (void *)&s_page);
    route(server, "/app.css", HTTP_GET, send_blob, (void *)&s_css);
    route(server, "/app.js", HTTP_GET, send_blob, (void *)&s_js);
    route(server, "/favicon.ico", HTTP_GET, favicon_get, NULL);
    route(server, "/stream", HTTP_GET, stream_get, NULL);
    route(server, "/snapshot", HTTP_GET, snapshot_get, NULL);
    ESP_ERROR_CHECK(http_api_register(server));

    ESP_LOGI(TAG, "Arayuz %u bayt, port %d",
             (unsigned)(s_page.end - s_page.start), CONFIG_WEBCAM_HTTP_PORT);
    return ESP_OK;
}
