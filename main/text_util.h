#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Hedef tamponu her zaman sonlandirir. */
static inline void copy_cstr(char *dst, size_t dst_len, const char *src)
{
    size_t i = 0;
    if (dst_len == 0) {
        return;
    }
    if (src) {
        while (src[i] && i + 1 < dst_len) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

/* 150 -> "15.0" . printf ile %f kullanmiyoruz; nano libc onu kirpabiliyor. */
static inline void fps_x10_to_text(char *dst, size_t dst_len, unsigned fps_x10)
{
    snprintf(dst, dst_len, "%u.%u", fps_x10 / 10, fps_x10 % 10);
}

static inline uint16_t fps_to_x10(float fps)
{
    if (fps <= 0.0f) {
        return 0;
    }
    int v = (int)(fps * 10.0f + 0.5f);
    if (v < 0) {
        v = 0;
    }
    if (v > 65535) {
        v = 65535;
    }
    return (uint16_t)v;
}

/* JSON icinde tirnak ve ters bolu kacar. Sigmazsa keser. */
static inline void json_escape(char *dst, size_t dst_len, const char *src)
{
    size_t o = 0;
    if (dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (size_t i = 0; src[i] && o + 2 < dst_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            continue;
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

/* "anahtar": "deger" ciftini okur. Kendi arayuzumuzun urettigi kucuk JSON icindir. */
static inline bool json_find_string(const char *json, const char *key, char *out, size_t out_len)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pat), '"');
    if (!p) {
        return false;
    }
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_len) {
        if (*p == '\\' && p[1]) {
            p++;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return *p == '"';
}

static inline bool json_find_number(const char *json, const char *key, float *out)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':') {
        p++;
    }
    char *end = NULL;
    float v = strtof(p, &end);
    if (end == p) {
        return false;
    }
    *out = v;
    return true;
}
