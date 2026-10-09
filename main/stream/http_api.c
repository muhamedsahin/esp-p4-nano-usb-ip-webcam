/*
 * http_api.c
 *
 * Sayfanin sordugu kisa JSON uclari.
 * Video soketi burada degil; o is http_server.c icindeki ayri gorevde.
 */

#include "http_api.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_timer.h"

#include "config.h"
#include "frame_hub.h"
#include "http_util.h"
#include "nvs_store.h"
#include "text_util.h"
#include "usb_camera.h"
#include "wifi_manager.h"

static const char *mode_name(wifi_link_mode_t mode)
{
    if (mode == WIFI_LINK_STA) {
        return "sta";
    }
    if (mode == WIFI_LINK_AP) {
        return "ap";
    }
    return "off";
}

static esp_err_t status_get(httpd_req_t *req)
{
    wifi_status_t wifi;
    cam_status_t cam;
    frame_stats_t hub;
    wifi_manager_get_status(&wifi);
    usb_camera_get_status(&cam);
    frame_hub_get_stats(&hub);

    char ssid[80];
    char host[40];
    json_escape(ssid, sizeof(ssid), wifi.ssid);
    json_escape(host, sizeof(host), wifi.hostname);

    char json[768];
    snprintf(json, sizeof(json),
             "{\"uptime_s\":%u,"
             "\"wifi\":{\"mode\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"hostname\":\"%s\",\"connected\":%s},"
             "\"camera\":{\"connected\":%s,\"streaming\":%s,\"width\":%u,\"height\":%u,"
             "\"fps_x10\":%u,\"error\":\"%s\",\"frames\":%u,\"overflows\":%u},"
             "\"hub\":{\"fps_x10\":%u,\"kbps\":%u,\"viewers\":%d,\"dropped\":%u,\"published\":%u}}",
             (unsigned)(esp_timer_get_time() / 1000000),
             mode_name(wifi.mode), ssid, wifi.ip, (int)wifi.rssi, host,
             wifi.connected ? "true" : "false",
             cam.connected ? "true" : "false",
             cam.streaming ? "true" : "false",
             cam.width, cam.height, cam.fps_x10, cam.error,
             (unsigned)cam.frames, (unsigned)cam.overflows,
             hub.fps_x10, (unsigned)hub.kbps, hub.viewers,
             (unsigned)hub.dropped, (unsigned)hub.published);
    return http_send_json(req, json);
}

static esp_err_t formats_get(httpd_req_t *req)
{
    cam_mode_t modes[CAM_MAX_MODES];
    int count = usb_camera_get_modes(modes, CAM_MAX_MODES);

    char json[1600];
    size_t used = (size_t)snprintf(json, sizeof(json), "{\"formats\":[");
    for (int i = 0; i < count; i++) {
        char fps[8];
        fps_x10_to_text(fps, sizeof(fps), modes[i].fps_x10);
        int wrote = snprintf(json + used, sizeof(json) - used,
                             "%s{\"w\":%u,\"h\":%u,\"fps\":%s}",
                             (i == 0) ? "" : ",",
                             modes[i].width, modes[i].height, fps);
        if (wrote < 0 || (size_t)wrote >= sizeof(json) - used) {
            break;
        }
        used += (size_t)wrote;
    }
    snprintf(json + used, sizeof(json) - used, "]}");
    return http_send_json(req, json);
}

static esp_err_t format_post(httpd_req_t *req)
{
    char body[128];
    if (http_read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Istek okunamadi");
        return ESP_FAIL;
    }

    float width = 0;
    float height = 0;
    float fps = 0;
    if (!json_find_number(body, "width", &width) || !json_find_number(body, "height", &height)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "width ve height gerekli");
        return ESP_FAIL;
    }
    json_find_number(body, "fps", &fps);

    esp_err_t err = usb_camera_set_mode((uint16_t)width, (uint16_t)height, fps_to_x10(fps));
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return http_send_json(req, "{\"ok\":false,\"error\":\"Kamera hazır değil\"}");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(req, "404 Not Found");
        return http_send_json(req, "{\"ok\":false,\"error\":\"Bu çözünürlük kamerada yok\"}");
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return http_send_json(req, "{\"ok\":false,\"error\":\"Çözünürlük uygulanamadı\"}");
    }

    cam_status_t cam;
    usb_camera_get_status(&cam);
    char fps_text[8];
    fps_x10_to_text(fps_text, sizeof(fps_text), cam.fps_x10);
    char json[96];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"width\":%u,\"height\":%u,\"fps\":%s}",
             cam.width, cam.height, fps_text);
    return http_send_json(req, json);
}

static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    wifi_scan_item_t items[WIFI_SCAN_MAX];
    int count = WIFI_SCAN_MAX;
    if (wifi_manager_scan(items, &count) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return http_send_json(req, "{\"ok\":false,\"networks\":[]}");
    }

    char json[1800];
    size_t used = (size_t)snprintf(json, sizeof(json), "{\"ok\":true,\"networks\":[");
    for (int i = 0; i < count; i++) {
        char ssid[80];
        json_escape(ssid, sizeof(ssid), items[i].ssid);
        int wrote = snprintf(json + used, sizeof(json) - used,
                             "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
                             (i == 0) ? "" : ",",
                             ssid, (int)items[i].rssi,
                             items[i].secure ? "true" : "false");
        if (wrote < 0 || (size_t)wrote >= sizeof(json) - used) {
            break;
        }
        used += (size_t)wrote;
    }
    snprintf(json + used, sizeof(json) - used, "]}");
    return http_send_json(req, json);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    char body[192];
    if (http_read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Istek okunamadi");
        return ESP_FAIL;
    }

    char ssid[33];
    char pass[65];
    if (!json_find_string(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return http_send_json(req, "{\"ok\":false,\"error\":\"Ağ adı gerekli\"}");
    }
    if (!json_find_string(body, "password", pass, sizeof(pass))) {
        pass[0] = '\0';
    }

    size_t pass_len = strlen(pass);
    if (pass_len > 0 && pass_len < 8) {
        httpd_resp_set_status(req, "400 Bad Request");
        return http_send_json(req, "{\"ok\":false,\"error\":\"Şifre en az 8 karakter olmalı. Açık ağ için boş bırak.\"}");
    }
    if (strlen(ssid) > 32 || pass_len > 63) {
        httpd_resp_set_status(req, "400 Bad Request");
        return http_send_json(req, "{\"ok\":false,\"error\":\"Ağ adı veya şifre çok uzun\"}");
    }

    if (nvs_store_save_wifi(ssid, pass) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return http_send_json(req, "{\"ok\":false,\"error\":\"Hafızaya yazılamadı\"}");
    }

    http_send_json(req, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

esp_err_t http_api_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/formats", .method = HTTP_GET, .handler = formats_get},
        {.uri = "/api/format", .method = HTTP_POST, .handler = format_post},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_get},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
