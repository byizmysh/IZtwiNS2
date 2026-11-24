/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef IZTWINS2_PREVIEW_CACHE_H
#define IZTWINS2_PREVIEW_CACHE_H

#include "account-types.h"
#include "preview.h"
#include <stdatomic.h>
#include <switch.h>

enum { PREVIEW_CACHE_SLOTS = 18 };

typedef struct {
    char path[THUMB_URL_BYTES];
    PreviewImage *image;
    uint64_t refresh_at;
} PreviewItem;

typedef struct {
    Thread thread;
    atomic_bool cancelled, finished;
    bool running;
    PreviewItem pending, items[PREVIEW_CACHE_SLOTS];
    unsigned next;
} PreviewCache;

bool previews_collect(PreviewCache *, uint64_t now);
void previews_request(PreviewCache *, const ChannelList *, unsigned first, unsigned visible, uint64_t now);
void previews_refresh_live(PreviewCache *, const ChannelList *, unsigned first, unsigned visible);
const PreviewImage *previews_find(const PreviewCache *, const char *url);
void previews_close(PreviewCache *);

#endif
