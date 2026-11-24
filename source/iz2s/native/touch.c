/* SPDX-License-Identifier: GPL-3.0-only */
#include "touch.h"
#include "ui-size.h"

#include <string.h>

void touch_layout_add(TouchLayout *layout, int x, int y, int width, int height, TouchKind kind, unsigned index) {
    if (!layout || layout->count >= sizeof(layout->entries) / sizeof(layout->entries[0]) ||
        width <= 0 || height <= 0) return;
    unsigned slot = layout->count++;
    layout->entries[slot].x = x;
    layout->entries[slot].y = y;
    layout->entries[slot].width = width;
    layout->entries[slot].height = height;
    layout->entries[slot].target = (TouchTarget){kind, index};
}

TouchTarget touch_layout_find(const TouchLayout *layout, int x, int y) {
    for (unsigned i = layout->count; i;) {
        --i;
        if (x >= layout->entries[i].x && x < layout->entries[i].x + layout->entries[i].width &&
            y >= layout->entries[i].y && y < layout->entries[i].y + layout->entries[i].height)
            return layout->entries[i].target;
    }
    return (TouchTarget){0};
}

TouchContact touch_contact(unsigned count, uint32_t flags, uint32_t id, unsigned x, unsigned y) {
    if (!count) return (TouchContact){.state = CONTACT_IDLE};
    if (count != 1 || x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT)
        return (TouchContact){.state = CONTACT_IGNORE};
    enum { TOUCH_END_FLAG = 2 };
    return (TouchContact){flags & TOUCH_END_FLAG ? CONTACT_END : CONTACT_DOWN, id, (int)x, (int)y};
}

TouchContact touch_iz2s_contact(const void *controls, size_t bytes) {
    /* The launcher publishes its one-frame touchscreen snapshot at controls+128.
       Coordinates use the same 1280x720 logical grid as the application. */
    enum { OFFSET = 128, COUNT = 2, FLAGS = 6, ID = 7, X = 8, Y = 9 };
    uint32_t sample[Y + 1];
    if (!controls || bytes < OFFSET + sizeof(sample)) return (TouchContact){0};
    memcpy(sample, (const unsigned char *)controls + OFFSET, sizeof(sample));
    return touch_contact(sample[COUNT], sample[FLAGS], sample[ID], sample[X], sample[Y]);
}

TouchEvent touch_update(TouchGesture *g, TouchContact contact, uint64_t now) {
    if (contact.state == CONTACT_IGNORE ||
        (g->tracking && contact.state != CONTACT_IDLE && contact.id != g->id)) {
        bool canceled = g->tracking;
        *g = (TouchGesture){.blocked = true};
        return (TouchEvent){.phase = canceled ? TOUCH_CANCEL : TOUCH_NO_EVENT};
    }
    if (g->blocked) {
        if (contact.state == CONTACT_IDLE) g->blocked = false;
        return (TouchEvent){0};
    }
    if (!g->tracking) {
        if (contact.state != CONTACT_DOWN) return (TouchEvent){0};
        *g = (TouchGesture){.start_x = contact.x, .start_y = contact.y, .x = contact.x,
                            .y = contact.y, .id = contact.id, .since = now, .tracking = true};
        return (TouchEvent){.phase = TOUCH_BEGIN, .x = contact.x, .y = contact.y};
    }

    bool moved = false;
    if (contact.state != CONTACT_IDLE) {
        moved = g->x != contact.x || g->y != contact.y;
        g->x = contact.x;
        g->y = contact.y;
    }
    int dx = g->x - g->start_x, dy = g->y - g->start_y;
    if (dx * dx + dy * dy >= 12 * 12) g->dragged = true;
    const bool release = contact.state != CONTACT_DOWN;
    if (release) g->tracking = false;
    return (TouchEvent){
        .phase = release ? TOUCH_RELEASE : moved ? TOUCH_MOVE : TOUCH_NO_EVENT,
        .x = g->x, .y = g->y, .dx = dx, .dy = dy, .dragged = g->dragged,
        .held = now - g->since >= UINT64_C(500000000),
    };
}
