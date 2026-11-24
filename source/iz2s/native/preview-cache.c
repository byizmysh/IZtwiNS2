/* SPDX-License-Identifier: GPL-3.0-only */
#include "preview-cache.h"
#include "preview.h"
#include "https.h"

#include <stdlib.h>
#include <string.h>

enum { IMAGE_CAP = 768 * 1024 };
static const uint64_t live_refresh_ns = UINT64_C(120000000000);
static const uint64_t retry_ns = UINT64_C(30000000000);

static unsigned find(const PreviewCache *cache, const char *path) {
    for (unsigned i = 0; i < PREVIEW_CACHE_SLOTS; ++i)
        if (!strcmp(cache->items[i].path, path)) return i;
    return PREVIEW_CACHE_SLOTS;
}

static bool live_path(const char *path) {
    return !strncmp(path, "https://static-cdn.jtvnw.net/previews-ttv/",
                    sizeof("https://static-cdn.jtvnw.net/previews-ttv/") - 1);
}

const PreviewImage *previews_find(const PreviewCache *cache, const char *url) {
    char path[THUMB_URL_BYTES];
    if (!preview_url(url, path, sizeof(path))) return NULL;
    unsigned index = find(cache, path);
    return index < PREVIEW_CACHE_SLOTS ? cache->items[index].image : NULL;
}

static bool get_preview(Https *net, const char *url, char *data, size_t *bytes) {
    if (https_image(net, url, data, IMAGE_CAP, bytes)) return true;
    const char suffix[] = "-480x270.jpg";
    size_t size = strlen(url), suffix_size = sizeof(suffix) - 1;
    if (!live_path(url) || size < suffix_size || strcmp(url + size - suffix_size, suffix)) return false;
    char fallback[THUMB_URL_BYTES];
    memcpy(fallback, url, size - suffix_size);
    strcpy(fallback + size - suffix_size, "-320x180.jpg");
    return https_image(net, fallback, data, IMAGE_CAP, bytes);
}

static void fetch(void *opaque) {
    PreviewCache *cache = opaque;
    char *data = malloc(IMAGE_CAP);
    size_t bytes = 0;
    Https net = {0};
    if (data && !atomic_load(&cache->cancelled) && https_begin(&net, &cache->cancelled) &&
        get_preview(&net, cache->pending.path, data, &bytes) && !atomic_load(&cache->cancelled))
        cache->pending.image = preview_decode((const unsigned char *)data, (unsigned)bytes);
    https_end(&net);
    free(data);
    atomic_store(&cache->finished, true);
}

static void complete(PreviewCache *cache, uint64_t now) {
    unsigned index = find(cache, cache->pending.path);
    if (index == PREVIEW_CACHE_SLOTS) {
        index = cache->next++ % PREVIEW_CACHE_SLOTS;
        free(cache->items[index].image);
        cache->items[index] = (PreviewItem){0};
        strcpy(cache->items[index].path, cache->pending.path);
    }
    PreviewItem *item = &cache->items[index];
    if (cache->pending.image) {
        free(item->image);
        item->image = cache->pending.image;
        item->refresh_at = live_path(item->path) ? now + live_refresh_ns : UINT64_MAX;
    } else item->refresh_at = now + retry_ns;
    cache->pending = (PreviewItem){0};
}

void previews_request(PreviewCache *cache, const ChannelList *rows, unsigned first, unsigned visible, uint64_t now) {
    if (cache->running) return;
    char path[THUMB_URL_BYTES];
    cache->pending.path[0] = 0;
    for (unsigned i = first; i < rows->count && i < first + visible; ++i) {
        if (!preview_url(rows->items[i].thumbnail, path, sizeof(path))) continue;
        unsigned index = find(cache, path);
        if (index == PREVIEW_CACHE_SLOTS) {
            strcpy(cache->pending.path, path);
            break;
        }
        if (!*cache->pending.path && now >= cache->items[index].refresh_at)
            strcpy(cache->pending.path, path);
    }
    if (!*cache->pending.path) return;
    atomic_store(&cache->cancelled, false);
    atomic_store(&cache->finished, false);
    if (R_FAILED(threadCreate(&cache->thread, fetch, cache, NULL, 0x30000, 0x32, -2))) {
        complete(cache, now);
        return;
    }
    if (R_FAILED(threadStart(&cache->thread))) {
        threadClose(&cache->thread);
        complete(cache, now);
        return;
    }
    cache->running = true;
}

static void join(PreviewCache *cache) {
    threadWaitForExit(&cache->thread);
    threadClose(&cache->thread);
    cache->running = false;
}

bool previews_collect(PreviewCache *cache, uint64_t now) {
    if (!cache->running || !atomic_load(&cache->finished)) return false;
    join(cache);
    bool loaded = cache->pending.image != NULL;
    if (!atomic_load(&cache->cancelled)) complete(cache, now);
    else {
        free(cache->pending.image);
        cache->pending = (PreviewItem){0};
        loaded = false;
    }
    return loaded;
}

void previews_refresh_live(PreviewCache *cache, const ChannelList *rows, unsigned first, unsigned visible) {
    for (unsigned i = first; i < rows->count && i < first + visible; ++i) {
        char path[THUMB_URL_BYTES];
        if (!preview_url(rows->items[i].thumbnail, path, sizeof(path)) || !live_path(path)) continue;
        unsigned index = find(cache, path);
        if (index < PREVIEW_CACHE_SLOTS) cache->items[index].refresh_at = 0;
    }
}

void previews_close(PreviewCache *cache) {
    if (cache->running) {
        atomic_store(&cache->cancelled, true);
        join(cache);
    }
    free(cache->pending.image);
    for (unsigned i = 0; i < PREVIEW_CACHE_SLOTS; ++i) free(cache->items[i].image);
}
