/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef IZTWINS2_PREVIEW_H
#define IZTWINS2_PREVIEW_H
#include <stdbool.h>
#include <stdint.h>

enum { PREVIEW_MAX_WIDTH = 480, PREVIEW_MAX_HEIGHT = 320, PREVIEW_COLUMNS = 3, PREVIEW_VISIBLE = 6 };
typedef struct {
    unsigned width, height;
    uint8_t pixels[];
} PreviewImage;

bool preview_url(const char *url, char *normalized, unsigned capacity);
PreviewImage *preview_decode(const unsigned char *data, unsigned bytes);
/* Coordinates are unsigned 8-bit fixed point, clipped to the source bounds. */
uint32_t preview_pixel(const PreviewImage *, unsigned x, unsigned y);
#endif
