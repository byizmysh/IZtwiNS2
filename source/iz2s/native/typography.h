/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef IZTWINS2_TYPOGRAPHY_H
#define IZTWINS2_TYPOGRAPHY_H
#include <stddef.h>
#include <stdint.h>

typedef enum { SMALL, BODY, LABEL, CARD, TITLE, HERO, CAPTION_LARGE, CAPTION_XL, UI_LABEL, FONT_STYLES } Font;

int typography_width(Font face, const char *text);
size_t typography_break(Font face, const char *text, int width);
void typography_draw(uint32_t *target, unsigned stride, int canvas_width, int canvas_height,
                     int x, int baseline, int width, Font face, const char *text, uint32_t color);
void typography_close(void);

#endif
