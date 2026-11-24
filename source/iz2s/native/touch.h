/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef IZTWINS2_TOUCH_H
#define IZTWINS2_TOUCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    HIT_NONE, HIT_MENU_SECTION, HIT_MENU_TOOL, HIT_MENU_FILTER, HIT_MENU_LIST, HIT_MENU_CARD,
    HIT_MENU_PAGE, HIT_KEY, HIT_PLAYER_ACTION, HIT_SETTING, HIT_QUALITY,
    HIT_TIMELINE, HIT_PANEL, HIT_CHAT_BODY, HIT_CHAT_BACK, HIT_CHAT_LIVE,
    HIT_CHAT_MESSAGE,
} TouchKind;

typedef enum {
    KEY_SELECT, KEY_BACK, KEY_ACCOUNT, KEY_SEARCH, KEY_REFRESH, KEY_CHANNEL,
    KEY_SAVE, KEY_UP, KEY_DOWN, KEY_COUNT,
} TouchKey;

typedef struct {
    TouchKind kind;
    unsigned index;
} TouchTarget;

typedef struct {
    struct {
        int x, y, width, height;
        TouchTarget target;
    } entries[48];
    unsigned count;
} TouchLayout;

typedef enum { CONTACT_IDLE, CONTACT_DOWN, CONTACT_END, CONTACT_IGNORE } ContactState;
typedef struct {
    ContactState state;
    uint32_t id;
    int x, y;
} TouchContact;

typedef enum { TOUCH_NO_EVENT, TOUCH_BEGIN, TOUCH_MOVE, TOUCH_RELEASE, TOUCH_CANCEL } TouchPhase;
typedef struct {
    TouchPhase phase;
    int x, y, dx, dy;
    bool dragged, held;
} TouchEvent;

typedef struct {
    int start_x, start_y, x, y;
    uint32_t id;
    uint64_t since;
    bool tracking, blocked, dragged;
} TouchGesture;

void touch_layout_add(TouchLayout *, int x, int y, int width, int height, TouchKind, unsigned index);
TouchTarget touch_layout_find(const TouchLayout *, int x, int y);
TouchContact touch_contact(unsigned count, uint32_t flags, uint32_t id, unsigned x, unsigned y);
TouchContact touch_iz2s_contact(const void *controls, size_t bytes);
TouchEvent touch_update(TouchGesture *, TouchContact, uint64_t now);

#endif
