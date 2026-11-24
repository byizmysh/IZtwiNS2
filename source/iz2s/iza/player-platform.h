/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef IZTWINS2_PLAYER_PLATFORM_H
#define IZTWINS2_PLAYER_PLATFORM_H
#include "../native/menu.h"
#include "../native/media.h"
#include <stdbool.h>
#include <stdint.h>

/* Word offsets shared with app.frag. The launcher waits for its previous
 * Vulkan submission before invoking the next native frame callback. */
enum {
    UI_OFFSET = 0x1000,
    VIDEO_OFFSET = 0x800000,
    PRIVATE_BYTES = 0xc00000,
};

typedef struct {
    uint32_t playback;
    uint32_t width, height, left, top, draw_width, draw_height;
    uint32_t chat, format;
} VideoSurface;

typedef struct {
    uint32_t *pixels, *video;
    VideoSurface *surface;
    unsigned position_tick, duration, quality;
    bool chat_failed, chat_can_send, caption_seen;
    char caption[CAPTION_BYTES];
    char caption_track[96];
} PlayerDisplay;

void iztwi_player_start(bool have_storage, uint64_t held);
bool iztwi_player_frame(PlayerDisplay *display, uint64_t held, TouchContact contact);
void iztwi_player_request_stop(void);
bool iztwi_player_workers_stopped(void);
void iztwi_player_stop(void);
#endif
