/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef IZTWINS2_PIXELS_H
#define IZTWINS2_PIXELS_H
#include <stdint.h>

/* The destination is premultiplied RGBA. Drawing to an opaque canvas is
 * the common case and uses the same arithmetic as drawing to a layer. */
static inline uint32_t over_opaque(uint32_t destination, uint32_t color, unsigned coverage) {
    const unsigned inverse = 255 - coverage;
    unsigned r = ((destination & 255) * inverse + (color & 255) * coverage + 127) / 255;
    unsigned g = ((destination >> 8 & 255) * inverse + (color >> 8 & 255) * coverage + 127) / 255;
    unsigned b = ((destination >> 16 & 255) * inverse + (color >> 16 & 255) * coverage + 127) / 255;
    unsigned a = coverage + ((destination >> 24) * inverse + 127) / 255;
    return r | g << 8 | b << 16 | a << 24;
}
#endif
