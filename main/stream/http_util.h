#pragma once

#include <stddef.h>
#include <string.h>

#include "esp_http_server.h"

static inline esp_err_t http_send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static inline int http_read_body(httpd_req_t *req, char *buf, size_t buflen)
{
    if (buflen < 2 || req->content_len == 0 || req->content_len >= buflen) {
        return -1;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        got += (size_t)n;
    }
    buf[got] = '\0';
    return (int)got;
}
