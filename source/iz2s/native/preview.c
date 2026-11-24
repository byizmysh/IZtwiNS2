/* SPDX-License-Identifier: GPL-3.0-only */
#include "preview.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_MAX_DIMENSIONS 2048
#include "stb_image.h"

bool preview_url(const char *url, char *normalized, unsigned capacity) {
    static const char static_host[] = "https://static-cdn.jtvnw.net";
    static const char clips_host[] = "https://clips-media-assets2.twitch.tv";
    const size_t host = !strncmp(url, static_host, sizeof(static_host) - 1) ? sizeof(static_host) - 1 :
                        !strncmp(url, clips_host, sizeof(clips_host) - 1) ? sizeof(clips_host) - 1 : 0;
    if (!host || url[host] != '/' || url[host + 1] == '/' || host >= capacity) return false;

    const char *path = url + host;
    const bool live = !strncmp(path, "/previews-ttv/", sizeof("/previews-ttv/") - 1);
    const bool box = !strncmp(path, "/ttv-boxart/", sizeof("/ttv-boxart/") - 1);
    if (host == sizeof(static_host) - 1 && !live && !box &&
        strncmp(path, "/cf_vods/", sizeof("/cf_vods/") - 1) &&
        strncmp(path, "/twitch-video-assets/", sizeof("/twitch-video-assets/") - 1) &&
        strncmp(path, "/jtv_user_pictures/", sizeof("/jtv_user_pictures/") - 1) &&
        strncmp(path, "/user-default-pictures-uv/", sizeof("/user-default-pictures-uv/") - 1)) return false;

    unsigned used = (unsigned)host;
    memcpy(normalized, url, host);
    bool width = false, height = false;
    while (*path) {
        const char *part = path;
        unsigned count = 1, skip = 1;
        const unsigned marker = *path == '%' && path[1] == '{';
        if (!width && !strncmp(path + marker, "{width}", 7)) {
            part = live ? "480" : box ? "240" : "320";
            count = 3;
            skip = 7 + marker;
            width = true;
        } else if (!height && !strncmp(path + marker, "{height}", 8)) {
            part = live ? "270" : box ? "320" : "180";
            count = 3;
            skip = 8 + marker;
            height = true;
        } else if (*path == '%' && isxdigit((unsigned char)path[1]) && isxdigit((unsigned char)path[2])) {
            if ((!strncmp(path, "%2f", 3) || !strncmp(path, "%2F", 3)) ||
                (!strncmp(path, "%5c", 3) || !strncmp(path, "%5C", 3)) ||
                (!strncmp(path, "%2e", 3) || !strncmp(path, "%2E", 3)) ||
                path[1] == '0' || path[1] == '1') return false;
            count = skip = 3;
        } else if (!((*path >= 'A' && *path <= 'Z') || (*path >= 'a' && *path <= 'z') ||
                     (*path >= '0' && *path <= '9') || strchr("/_.-~", *path))) return false;
        if (used + count >= capacity) return false;
        memcpy(normalized + used, part, count);
        used += count;
        path += skip;
    }
    normalized[used] = 0;
    return width == height && used > host + 4 &&
           (!strcmp(normalized + used - 4, ".jpg") || !strcmp(normalized + used - 4, ".png")) &&
           !strstr(normalized + host, "/../") && !strstr(normalized + host, "/./");
}

static uint32_t sample(const uint8_t *data, unsigned width, unsigned height, unsigned x, unsigned y) {
    unsigned left = x >> 8, top = y >> 8;
    unsigned right = left + (left + 1 < width), bottom = top + (top + 1 < height);
    unsigned dx = x & 255, dy = y & 255;
    const uint8_t *a = data + (top * width + left) * 3;
    const uint8_t *b = data + (top * width + right) * 3;
    const uint8_t *c = data + (bottom * width + left) * 3;
    const uint8_t *d = data + (bottom * width + right) * 3;
    uint32_t pixel = 0xff000000u;
    for (unsigned i = 0; i < 3; ++i) {
        unsigned upper = a[i] * (256 - dx) + b[i] * dx;
        unsigned lower = c[i] * (256 - dx) + d[i] * dx;
        unsigned value = (upper * (256 - dy) + lower * dy + 32768) >> 16;
        pixel |= value << (i * 8);
    }
    return pixel;
}

uint32_t preview_pixel(const PreviewImage *image, unsigned x, unsigned y) {
    return sample(image->pixels, image->width, image->height, x, y);
}

PreviewImage *preview_decode(const unsigned char *data, unsigned bytes) {
    int source_width, source_height, channels;
    if (!stbi_info_from_memory(data, (int)bytes, &source_width, &source_height, &channels) ||
        source_width < 1 || source_height < 1 || source_width > 1920 || source_height > 1080) return NULL;
    uint8_t *rgb = stbi_load_from_memory(data, (int)bytes, &source_width, &source_height, &channels, 3);
    if (!rgb) return NULL;
    unsigned width = (unsigned)source_width, height = (unsigned)source_height;
    if (width > PREVIEW_MAX_WIDTH) {
        height = height * PREVIEW_MAX_WIDTH / width;
        width = PREVIEW_MAX_WIDTH;
    }
    if (height > PREVIEW_MAX_HEIGHT) {
        width = width * PREVIEW_MAX_HEIGHT / height;
        height = PREVIEW_MAX_HEIGHT;
    }
    if (!width) width = 1;
    if (!height) height = 1;
    PreviewImage *image = malloc(sizeof(*image) + (size_t)width * height * 3);
    if (image) {
        image->width = width;
        image->height = height;
        if (width == (unsigned)source_width && height == (unsigned)source_height)
            memcpy(image->pixels, rgb, (size_t)width * height * 3);
        else for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
            uint32_t pixel = sample(rgb, source_width, source_height,
                                    x * (unsigned)source_width * 256 / width,
                                    y * (unsigned)source_height * 256 / height);
            uint8_t *dst = image->pixels + (y * width + x) * 3;
            for (unsigned i = 0; i < 3; ++i) dst[i] = (uint8_t)(pixel >> (8 * i));
        }
    }
    stbi_image_free(rgb);
    return image;
}
