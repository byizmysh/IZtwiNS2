/* SPDX-License-Identifier: GPL-3.0-only */
#include "menu.h"
#include "pixels.h"
#include "brand.h"
#include "media.h"
#include "typography.h"
#include "qrcodegen.h"

#include <stdio.h>
#include <string.h>

#define RGB(r,g,b) (UINT32_C(0xff000000) | (uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16)
#define BG RGB(13,13,22)
#define RAIL RGB(19,19,29)
#define PANEL RGB(26,26,40)
#define LINE RGB(48,47,65)
#define INK RGB(249,248,253)
#define DIM RGB(187,186,206)
#define ACCENT RGB(202,173,255)
#define FOCUS RGB(60,44,92)
#define CONTROL RGB(37,36,52)
#define ACTION_INK RGB(32,20,51)

_Static_assert(GLYPH_SIZE == 26 * UI_SCALE_NUM / UI_SCALE_DEN, "Control art must match UI density");

typedef struct { uint32_t *pixels; unsigned stride; int width; TouchLayout *touch; } Canvas;
typedef struct { int x, y, w, h; } Rect;

static void hit(Canvas c, Rect r, TouchKind kind, unsigned index) {
    touch_layout_add(c.touch, r.x, r.y, r.w, r.h, kind, index);
}

static Rect pixel_rect(Rect r) {
    int x = ui_px(r.x), y = ui_px(r.y);
    return (Rect){x, y, ui_px(r.x + r.w) - x, ui_px(r.y + r.h) - y};
}

static uint32_t mix(uint32_t from, uint32_t to, unsigned alpha) {
    return over_opaque(from, to, alpha);
}

static unsigned coverage(int x, int y, int w, int h, int radius) {
    if (x < 0 || y < 0 || x >= w || y >= h) return 0;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    int dx = x < radius ? radius - x : x >= w - radius ? x - (w - radius) + 1 : 0;
    int dy = y < radius ? radius - y : y >= h - radius ? y - (h - radius) + 1 : 0;
    if (!dx || !dy) return 255;
    int squared = radius * radius;
    if (dx * dx + dy * dy <= squared) return 255;
    int near_x = dx - 1, near_y = dy - 1;
    if (near_x * near_x + near_y * near_y >= squared) return 0;
    unsigned inside = 0;
    for (int j = 1; j < 16; j += 2)
        for (int i = 1; i < 16; i += 2) {
            int sx = near_x * 16 + i, sy = near_y * 16 + j;
            inside += sx * sx + sy * sy <= squared * 256;
        }
    return (inside * 255 + 32) / 64;
}

static void shape(Canvas c, Rect r, int radius, uint32_t color) {
    r = pixel_rect(r);
    radius = ui_px(radius);
    int left = r.x < 0 ? 0 : r.x, top = r.y < 0 ? 0 : r.y;
    int width = ui_px(c.width);
    int right = r.x + r.w > width ? width : r.x + r.w;
    int bottom = r.y + r.h > UI_HEIGHT ? UI_HEIGHT : r.y + r.h;
    for (int y = top; y < bottom; ++y) {
        uint32_t *row = c.pixels + y * c.stride;
        int ry = y - r.y;
        if (!radius || (ry >= radius && ry < r.h - radius)) {
            for (int x = left; x < right; ++x) row[x] = color;
        } else for (int x = left; x < right; ++x) {
            unsigned alpha = coverage(x - r.x, ry, r.w, r.h, radius);
            if (alpha == 255) row[x] = color;
            else if (alpha) row[x] = mix(row[x], color, alpha);
        }
    }
}

static void gradient(Canvas c, Rect r, int radius, uint32_t a, uint32_t b) {
    r = pixel_rect(r);
    radius = ui_px(radius);
    for (int y = 0; y < r.h; ++y) {
        uint32_t *row = c.pixels + (r.y + y) * c.stride + r.x;
        for (int x = 0; x < r.w; ++x) {
            unsigned alpha = coverage(x, y, r.w, r.h, radius);
            if (!alpha) continue;
            uint32_t color = mix(a, b, (unsigned)(x * 192 / r.w + y * 62 / r.h));
            row[x] = alpha == 255 ? color : mix(row[x], color, alpha);
        }
    }
}

static int text_width(Font face, const char *value) { return typography_width(face, value); }

static void text(Canvas c, int x, int baseline, int width, Font face, const char *value, uint32_t color) {
    int left = ui_px(x);
    typography_draw(c.pixels, c.stride, ui_px(c.width), UI_HEIGHT, left, ui_px(baseline),
                    ui_px(x + width) - left, face, value, color);
}

static void wrapped(Canvas c, int x, int baseline, int width, Font face, const char *value,
                    uint32_t ink, unsigned lines, int gap) {
    while (lines-- && *value) {
        size_t bytes = typography_break(face, value, width);
        if (!lines || !bytes || !value[bytes]) {
            text(c, x, baseline, width, face, value, ink);
            return;
        }
        char first[513];
        if (bytes >= sizeof(first)) bytes = sizeof(first) - 1;
        memcpy(first, value, bytes);
        first[bytes] = 0;
        text(c, x, baseline, width, face, first, ink);
        value += bytes;
        while (*value == ' ') ++value;
        baseline += gap;
    }
}

static void glyph(Canvas c, int center_x, int center_y, Glyph symbol, uint32_t color) {
    int left = ui_px(center_x) - GLYPH_SIZE / 2, top = ui_px(center_y) - GLYPH_SIZE / 2;
    const unsigned char *mask = glyph_alpha[symbol];
    int limit = ui_px(c.width);
    for (int y = 0; y < GLYPH_SIZE; ++y) {
        if (top + y < 0 || top + y >= UI_HEIGHT) continue;
        uint32_t *row = c.pixels + (top + y) * c.stride;
        for (int x = 0; x < GLYPH_SIZE; ++x) {
            unsigned alpha = mask[y * GLYPH_SIZE + x];
            if (alpha && left + x >= 0 && left + x < limit)
                row[left + x] = mix(row[left + x], color, alpha);
        }
    }
}

enum { RAIL_WIDTH = 220, CONTENT_X = 252, CONTENT_RIGHT = 1244, CONTENT_WIDTH = 992,
       ROW_GAP = 16, CARD_WIDTH = 320, CARD_STEP = 336, CARD_HEIGHT = 224,
       GRID_TOP = 180, CARD_INSET = 10, IMAGE_WIDTH = CARD_WIDTH - 2 * CARD_INSET, IMAGE_HEIGHT = 169 };

const MenuTab menu_root_tabs[MENU_ROOT_TABS] = {POPULAR, TRENDS, FOLLOWING, CONTINUE_WATCHING, FAVORITES, RECENT};
static const char *root_names[MENU_ROOT_TABS] = {"Home", "Trends", "Following", "Continue", "Favorites", "Recent"};

static void centered(Canvas, int, int, int, Font, const char *, uint32_t);
static void right_aligned(Canvas, int, int, int, Font, const char *, uint32_t);
static void rounded_border(Canvas, Rect, int, int, uint32_t, uint32_t);
static void playback_time(char out[24], unsigned ms);

static void bitmap_icon(Canvas c, int left, int top, int width, int height, const unsigned char *pixels) {
    left = ui_px(left);
    top = ui_px(top);
    for (int y = 0; y < height; ++y) {
        uint32_t *target = c.pixels + (top + y) * c.stride + left;
        for (int x = 0; x < width; ++x) {
            const unsigned char *p = pixels + (y * width + x) * 4;
            if (p[3]) target[x] = mix(target[x], RGB(p[0],p[1],p[2]), p[3]);
        }
    }
}

static void control_symbol(Canvas c, int left, int top, unsigned index) {
    bitmap_icon(c, left, top, CONTROL_ICON_SIZE, CONTROL_ICON_SIZE, control_rgba[index]);
}

static int key_width(const char *label) {
    int width = text_width(UI_LABEL, label) + 14;
    return width > 24 ? width : 24;
}

static void keycap(Canvas c, int x, int y, const char *label, uint32_t fill, uint32_t ink) {
    int width = key_width(label);
    shape(c, (Rect){x, y, width, 24}, 12, fill);
    centered(c, x, width, y + 18, UI_LABEL, label, ink);
}

static int command_button(Canvas c, int x, int y, const char *key, const char *label,
                          Glyph symbol, bool primary) {
    int lead = symbol == GLYPH_NONE ? 14 : 34;
    int label_width = text_width(LABEL, label);
    int width = lead + label_width + key_width(key) + 22;
    uint32_t ink = primary ? ACTION_INK : INK;
    shape(c, (Rect){x, y, width, 38}, 11, primary ? ACCENT : CONTROL);
    if (symbol != GLYPH_NONE) glyph(c, x + 19, y + 19, symbol, ink);
    text(c, x + lead, y + 26, label_width + 3, LABEL, label, ink);
    keycap(c, x + width - key_width(key) - 8, y + 7, key, primary ? RGB(171,139,236) : RAIL, ink);
    return x + width + 12;
}

static int shortcut(Canvas c, int x, int y, const char *buttons, const char *label) {
    int width = key_width(buttons);
    int length = text_width(SMALL, label);
    if (x + width + length + 12 > c.width) return x;
    keycap(c, x, y, buttons, CONTROL, INK);
    text(c, x + width + 8, y + 18, length + 4, SMALL, label, DIM);
    return x + width + length + 30;
}

static int shortcut_hit(Canvas c, int x, int y, const char *key, const char *label,
                        TouchKind kind, unsigned index) {
    int next = shortcut(c, x, y, key, label);
    if (next > x) hit(c, (Rect){x - 4, y - 5, next - x - 14, 34}, kind, index);
    return next;
}

static int right_shortcut(Canvas c, int right, int y, const char *key, const char *label, TouchKey action) {
    int left = right - key_width(key) - 8 - text_width(SMALL, label);
    shortcut_hit(c, left, y, key, label, HIT_KEY, action);
    return left - 20;
}

static void toolbar_button(Canvas c, const MenuFrame *view, MenuTool tool, Rect area,
                           const char *button, const char *label) {
    hit(c, area, HIT_MENU_TOOL, tool);
    bool focus = view->focus == MENU_TOOLBAR && view->tool == (unsigned)tool;
    shape(c, area, 13, focus ? ACCENT : PANEL);
    control_symbol(c, area.x + 7, area.y + 6, MENU_ROOT_TABS + (unsigned)tool);
    int key_x = area.x + area.w - key_width(button) - 10;
    uint32_t ink = focus ? ACTION_INK : INK;
    keycap(c, key_x, area.y + 11, button, focus ? RGB(171,139,236) : RAIL, ink);
    text(c, area.x + 51, area.y + 30, key_x - area.x - 63, LABEL, label, ink);
}

static void shell(Canvas c, const MenuFrame *view) {
    shape(c, (Rect){0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT}, 0, BG);
    shape(c, (Rect){0, 0, RAIL_WIDTH, DISPLAY_HEIGHT}, 0, RAIL);
    shape(c, (Rect){RAIL_WIDTH - 1, 0, 1, DISPLAY_HEIGHT}, 0, LINE);
    bitmap_icon(c, 24, 31, BRAND_WIDTH, BRAND_HEIGHT, brand_rgba);
    for (unsigned i = 0; i < MENU_ROOT_TABS; ++i) {
        MenuTab tab = menu_root_tabs[i];
        int y = 117 + (int)i * 58;
        hit(c, (Rect){16, y, 188, 50}, HIT_MENU_SECTION, i);
        bool active = view->root_tab == tab;
        bool focus = view->focus == MENU_RAIL && view->rail == i;
        if (active || focus) shape(c, (Rect){16, y, 188, 50}, 13, focus ? ACCENT : FOCUS);
        if (active) shape(c, (Rect){20, y + 16, 3, 18}, 1, focus ? ACTION_INK : ACCENT);
        control_symbol(c, 30, y + 8, i);
        text(c, 77, y + 32, 113, LABEL, root_names[i], focus ? ACTION_INK : active ? INK : DIM);
    }
    shape(c, (Rect){22, 659, 176, 1}, 0, LINE);
    if (view->touch_input)
        text(c, 24, 690, 178, SMALL, "Hold for card actions", DIM);
    else shortcut(c, 24, 672, menu_root_tab(view->tab) ? "L/R" : "Left", "Sections");

    toolbar_button(c, view, TOOL_SEARCH, (Rect){CONTENT_X, 28, 400, 46}, "Y",
                   view->tab == CHANNEL_CLIPS ? "Filter clips" : "Search Twitch");
    toolbar_button(c, view, TOOL_LINK, (Rect){668, 28, 260, 46}, "ZR", "Open link");
    toolbar_button(c, view, TOOL_ACCOUNT, (Rect){944, 28, 300, 46}, "X",
                   *view->login ? view->login : view->restoring_session ? "Saved session" : "Sign in");
    shape(c, (Rect){CONTENT_X, 89, CONTENT_WIDTH, 1}, 0, LINE);
}

static void thumb_image(Canvas c, Rect r, const PreviewImage *image) {
    r = pixel_rect(r);
    unsigned crop_width = image->width, crop_height = image->height;
    if ((unsigned)r.w * crop_height < (unsigned)r.h * crop_width)
        crop_width = (unsigned)r.w * crop_height / (unsigned)r.h;
    else
        crop_height = (unsigned)r.h * crop_width / (unsigned)r.w;
    if (!crop_width) crop_width = 1;
    if (!crop_height) crop_height = 1;
    unsigned left = (image->width - crop_width) * 128;
    unsigned top = (image->height - crop_height) * 128;
    static unsigned samples[UI_WIDTH];
    for (int x = 0; x < r.w; ++x)
        samples[x] = left + (unsigned)x * crop_width * 256 / (unsigned)r.w;
    for (int y = 0; y < r.h; ++y) {
        unsigned sy = top + (unsigned)y * crop_height * 256 / (unsigned)r.h;
        uint32_t *target = c.pixels + (r.y + y) * c.stride + r.x;
        for (int x = 0; x < r.w; ++x) {
            unsigned alpha = coverage(x, y, r.w, r.h, ui_px(12));
            if (!alpha) continue;
            uint32_t color = preview_pixel(image, samples[x], sy);
            target[x] = alpha == 255 ? color : mix(target[x], color, alpha);
        }
    }
}

static void viewer_count(char out[24], unsigned viewers) {
    unsigned divisor = viewers >= 1000000 ? 1000000 : 1000;
    if (viewers >= 1000)
        snprintf(out, 24, "%u.%u%s", viewers / divisor,
            viewers % divisor / (divisor / 10), divisor == 1000000 ? "M" : "K");
    else snprintf(out, 24, "%u", viewers);
}

static void thumbnail(Canvas c, Rect r, const Channel *row, const PreviewImage *image) {
    if (image) { thumb_image(c, r, image); return; }
    static const uint32_t colors[] = {RGB(58,36,95), RGB(40,64,78), RGB(74,38,77), RGB(48,57,99)};
    unsigned seed = (unsigned char)(*row->login ? row->login[0] : row->name[0]);
    gradient(c, r, 12, RGB(30,27,51), colors[seed % 4]);
    int middle = r.x + r.w / 2, center = r.y + r.h / 2;
    shape(c, (Rect){middle - 19, center - 19, 38, 38}, 19, RGB(34,28,54));
    glyph(c, middle, center, row->kind == ITEM_CATEGORY ? GLYPH_TRENDS : GLYPH_PLAY, ACCENT);
}

static void live_badge(Canvas c, int x, int y) {
    shape(c, (Rect){x, y, 63, 24}, 8, RGB(148,39,69));
    shape(c, (Rect){x + 8, y + 9, 6, 6}, 3, RGB(255,169,186));
    text(c, x + 20, y + 18, 38, UI_LABEL, "LIVE", INK);
}

static const char *channel_name(const Channel *row) { return *row->name ? row->name : row->login; }

static void category_card(Canvas c, const Channel *row, const PreviewImage *poster, int x, int y, bool focus) {
    Rect cover = {x + 10, y + 10, 141, 199};
    thumbnail(c, cover, row, poster);
    wrapped(c, x + 167, y + 75, 140, LABEL, channel_name(row), INK, 2, 25);
    if (row->viewers) {
        char count[24], label[40];
        viewer_count(count, row->viewers);
        snprintf(label, sizeof(label), "%s watching", count);
        text(c, x + 167, y + 147, 140, SMALL, label, ACCENT);
    }
    if (focus) command_button(c, x + 166, y + 169, "A", "Open", GLYPH_RIGHT, true);
}

static void card_outline(Canvas c, Rect card, bool focus) {
    if (focus) rounded_border(c, card, 16, 2, ACCENT, RGB(43,34,65));
    else shape(c, card, 16, PANEL);
}

static void stream_card(Canvas c, const MenuFrame *view, unsigned index, unsigned position, int x, int y) {
    const Channel *row = &view->list->items[index];
    const bool focus = view->focus == MENU_GRID && index == view->selected;
    Rect area = {x, y, CARD_WIDTH, CARD_HEIGHT};
    hit(c, area, HIT_MENU_CARD, index);
    card_outline(c, area, focus);
    if (row->kind == ITEM_CATEGORY) {
        category_card(c, row, view->previews[position], x, y, focus);
        return;
    }
    Rect screen = {x + CARD_INSET, y + CARD_INSET, IMAGE_WIDTH, IMAGE_HEIGHT};
    const PreviewImage *preview = view->previews[position];
    if (row->kind == ITEM_CHANNEL && preview && preview->width <= preview->height) {
        gradient(c, screen, 12, RGB(27,24,45), RGB(55,41,80));
        thumbnail(c, (Rect){x + (CARD_WIDTH - 135) / 2, y + 27, 135, 135}, row, preview);
    } else thumbnail(c, screen, row, preview);
    if (row->kind == ITEM_LIVE && (row->viewers || *row->title)) live_badge(c, x + 15, y + 14);
    else if (row->kind != ITEM_LIVE) {
        const char *kind = row->kind == ITEM_CHANNEL ? (row->status_known ? "Offline" : "Channel") :
                           row->kind == ITEM_VIDEO ? "Video" : "Clip";
        int width = text_width(SMALL, kind) + 18;
        shape(c, (Rect){x + 15, y + 14, width, 24}, 8, RAIL);
        text(c, x + 24, y + 32, width - 18, SMALL, kind, INK);
    }
    if (row->viewers || view->duration[position]) {
        char value[24];
        if (row->viewers) viewer_count(value, row->viewers);
        else playback_time(value, view->duration[position]);
        int width = text_width(SMALL, value) + 16;
        int rx = screen.x + screen.w - 9 - width, by = screen.y + screen.h - 32;
        shape(c, (Rect){rx, by, width, 24}, 8, RAIL);
        centered(c, rx, width, by + 18, SMALL, value, INK);
    }
    if (view->progress[position] && view->duration[position]) {
        unsigned progress = view->progress[position], duration = view->duration[position];
        if (progress > duration) progress = duration;
        int left = screen.x + 10, width = screen.w - 20, top = screen.y + screen.h - 7;
        shape(c, (Rect){left, top, width, 3}, 1, LINE);
        int fill = (int)((uint64_t)progress * width / duration);
        if (fill) shape(c, (Rect){left, top, fill, 3}, 1, ACCENT);
    }
    if (view->saved[position]) {
        shape(c, (Rect){x + CARD_WIDTH - 45, y + 14, 30, 26}, 7, RAIL);
        glyph(c, x + CARD_WIDTH - 30, y + 27, GLYPH_FAVORITES, ACCENT);
    }
    if (focus) {
        const bool channel = row->kind == ITEM_CHANNEL;
        const bool resume = view->progress[position] && row->kind == ITEM_VIDEO;
        command_button(c, screen.x + 9, screen.y + screen.h - 50, "A",
                       channel ? "Open" : resume ? "Resume" : "Watch", channel ? GLYPH_RIGHT : GLYPH_PLAY, true);
    }
    const char *name = channel_name(row);
    const char *title = *row->title ? row->title : *name ? name : row->id;
    text(c, x + 14, y + 199, CARD_WIDTH - 28, LABEL, title, INK);
    if (*row->title && *name) text(c, x + 14, y + 216, CARD_WIDTH - 28, SMALL, name, DIM);
    else if (*row->login && strcmp(row->login, title))
        text(c, x + 14, y + 216, CARD_WIDTH - 28, SMALL, row->login, DIM);
}

static void featured(Canvas c, const MenuFrame *view, unsigned index) {
    const Channel *row = &view->list->items[index];
    bool focus = view->focus == MENU_GRID && view->selected == index;
    Rect card = {CONTENT_X, GRID_TOP, CONTENT_WIDTH, 208};
    hit(c, card, HIT_MENU_CARD, index);
    card_outline(c, card, focus);
    thumbnail(c, (Rect){CONTENT_X + 10, GRID_TOP + 10, 336, 189}, row, view->previews[0]);
    const int left = CONTENT_X + 372, w = CONTENT_RIGHT - left - 26;
    live_badge(c, left, 202);
    if (row->viewers) {
        char count[24], label[40];
        viewer_count(count, row->viewers);
        snprintf(label, sizeof(label), "%s watching", count);
        text(c, left + 78, 220, w - 80, SMALL, label, ACCENT);
    }
    wrapped(c, left, 263, w, CARD, *row->title ? row->title : channel_name(row), INK, 2, 30);
    text(c, left, 322, w, BODY, channel_name(row), DIM);
    if (focus) command_button(c, left, 336, "A", "Watch", GLYPH_PLAY, true);
}

static void blank(Canvas c, const MenuFrame *view) {
    const char *heading = "No results", *detail = "Try another search or open a channel link.";
    const char *button = "Y", *action = "Search Twitch";
    TouchKey key = KEY_SEARCH;
    if (view->loading) {
        heading = view->restoring_session && view->tab == FOLLOWING ? "Restoring Twitch session…" : "Loading Twitch…";
        detail = "Getting the latest list.";
        action = NULL;
    } else if (view->status && *view->status && view->tab != FAVORITES && view->tab != RECENT &&
               view->tab != CONTINUE_WATCHING) {
        heading = "This list is unavailable"; detail = view->status;
        button = "+"; action = "Refresh";
        key = KEY_REFRESH;
    } else if (view->tab == FOLLOWING && !*view->login) {
        heading = view->restoring_session ? "Restore your Twitch session" : "Follow your favorite channels";
        detail = "Sign in to see who is live.";
        button = view->restoring_session ? "A" : "X";
        key = view->restoring_session ? KEY_SELECT : KEY_ACCOUNT;
        action = view->restoring_session ? "Retry sign-in" : "Sign in";
    } else if (view->tab == FAVORITES) {
        heading = "No favorites yet"; detail = "Press − on a channel card to save it here.";
    } else if (view->tab == RECENT) {
        heading = "Nothing watched yet"; detail = "Streams appear here after you watch them.";
    } else if (view->tab == CONTINUE_WATCHING) {
        heading = "Continue watching"; detail = "Unfinished Twitch videos appear here with their playback position.";
    } else if (view->tab == FOLLOWING || view->tab == CATEGORY) {
        heading = "No live channels"; detail = "Come back later or search for another stream.";
    } else if (view->tab == CHANNEL_LIVE) {
        heading = "Channel is offline";
        detail = "Open Videos or Clips to keep watching.";
        button = "L/R"; action = "Channel tabs";
    } else if (view->tab == CHANNEL_VIDEOS || view->tab == CHANNEL_CLIPS) {
        heading = view->tab == CHANNEL_VIDEOS ? "No videos" : "No clips";
        detail = "Check the other section or return to browsing.";
        button = "L/R"; action = "Videos / clips";
        if (view->tab == CHANNEL_CLIPS && view->clip_filter && *view->clip_filter) {
            heading = "No matching clips on this page";
            detail = "Change the filter or browse another page.";
            button = "Y"; action = "Edit filter";
        }
    }
    shape(c, (Rect){CONTENT_X, 184, CONTENT_WIDTH, 456}, 18, PANEL);
    for (unsigned i = 0; i < MENU_ROOT_TABS; ++i)
        if (menu_root_tabs[i] == view->root_tab) control_symbol(c, CONTENT_X + 33, 219, i);
    text(c, CONTENT_X + 33, 314, CONTENT_WIDTH - 66, TITLE, heading, INK);
    wrapped(c, CONTENT_X + 35, 356, CONTENT_WIDTH - 70, BODY, detail, DIM, 2, 28);
    if (action && strcmp(button, "L/R")) {
        int x = CONTENT_X + 35;
        int next = command_button(c, x, 402, button, action, GLYPH_NONE, true);
        hit(c, (Rect){x, 402, next - x - 12, 38}, HIT_KEY, key);
    }
    else if (action) shortcut(c, CONTENT_X + 35, 408, button, action);
}

static void grid(Canvas c, const MenuFrame *view) {
    static const char *names[] = {"Following", "Home", "Favorites", "Recent", "Trends", "Continue watching",
                                  "Search", "Category", "Live", "Videos", "Clips"};
    const char *heading = view->heading && *view->heading ? view->heading : names[view->tab];
    text(c, CONTENT_X, 142, 801, TITLE, heading, INK);
    const char *detail = view->channel_note;
    if (view->tab == TRENDS) detail = "Categories ranked by live viewers";
    if (view->tab == SEARCH && view->query && *view->query) detail = view->query;
    if (view->tab == CHANNEL_CLIPS && view->clip_filter && *view->clip_filter) detail = view->clip_filter;
    if (view->tab == SEARCH || menu_profile_tab(view->tab)) {
        static const char *search_filters[] = {"All", "Live", "Channels", "Categories", "Videos"};
        static const char *channel_tabs[] = {"Live", "Videos", "Clips"};
        bool search = view->tab == SEARCH;
        int x = CONTENT_X;
        for (unsigned i = 0; i < (search ? (unsigned)SEARCH_FILTERS : 3u); ++i) {
            const char *label = search ? search_filters[i] : channel_tabs[i];
            int width = text_width(UI_LABEL, label) + 29;
            bool focus = view->focus == MENU_FILTERS && view->selector_hover == i;
            bool active = view->selector == i;
            Rect area = {x, 148, width, 27};
            hit(c, area, HIT_MENU_FILTER, i);
            shape(c, area, 11, focus ? ACCENT : active ? FOCUS : PANEL);
            centered(c, x, width, 167, UI_LABEL, label, focus ? ACTION_INK : active ? INK : DIM);
            x += width + 8;
        }
        if (detail && *detail) right_aligned(c, CONTENT_RIGHT, CONTENT_RIGHT - x - 12, 169, SMALL, detail, DIM);
    } else if (detail && *detail) text(c, CONTENT_X + 2, 169, CONTENT_WIDTH - 4, SMALL, detail, DIM);

    hit(c, (Rect){CONTENT_X, GRID_TOP, CONTENT_WIDTH, 467}, HIT_MENU_LIST, 0);
    if (!view->list->count) { blank(c, view); return; }
    unsigned size = menu_page_size(view->tab, view->selected);
    unsigned first = menu_page(view->tab, view->selected);
    char page[38];
    if (view->previous_page || view->next_page) snprintf(page, sizeof(page), "Page %u", view->page_number);
    else if (view->tab == POPULAR && first == 0)
        snprintf(page, sizeof(page), "%u live", view->list->count);
    else if (view->tab == POPULAR)
        snprintf(page, sizeof(page), "%u–%u / %u", first + 1,
                 first + size < view->list->count ? first + size : view->list->count, view->list->count);
    else snprintf(page, sizeof(page), "%u / %u", first / size + 1, (view->list->count + size - 1) / size);
    right_aligned(c, CONTENT_RIGHT, 116, 140, BODY, page, DIM);
    if (view->tab == POPULAR && first == 0) {
        featured(c, view, first);
        if (first + 1 < view->list->count) text(c, CONTENT_X, 414, CONTENT_WIDTH, CARD, "More live channels", INK);
        for (unsigned i = 1; i < size && first + i < view->list->count; ++i)
            stream_card(c, view, first + i, i, CONTENT_X + (int)(i - 1) * CARD_STEP, 424);
        return;
    }
    for (unsigned i = 0; i < size && first + i < view->list->count; ++i)
        stream_card(c, view, first + i, i, CONTENT_X + (int)(i % PREVIEW_COLUMNS) * CARD_STEP,
            GRID_TOP + (int)(i / PREVIEW_COLUMNS) * (CARD_HEIGHT + ROW_GAP));
}

static void qr(Canvas c, const char *link) {
    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)], result[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    if (!link || !*link || !qrcodegen_encodeText(link, tmp, result, qrcodegen_Ecc_MEDIUM, 1, 6,
                                                qrcodegen_Mask_AUTO, true)) return;
    int modules = qrcodegen_getSize(result);
    const int scale = 6, side = (modules + 8) * scale, left = 994 - side / 2, top = 357 - side / 2;
    shape(c, (Rect){left, top, side, side}, 0, INK);
    for (int y = 0; y < modules; ++y)
        for (int x = 0; x < modules; ++x)
            if (qrcodegen_getModule(result, x, y))
                shape(c, (Rect){left + (x + 4) * scale, top + (y + 4) * scale, scale, scale}, 0, BG);
}

static int modal_button(Canvas c, int x, const char *label, Glyph symbol, TouchKey key) {
    int next = command_button(c, x, 508, key == KEY_SELECT ? "A" : "B", label, symbol, key == KEY_SELECT);
    hit(c, (Rect){x, 508, next - x - 12, 38}, HIT_KEY, key);
    return next;
}

static void dialog(Canvas c, const MenuFrame *view) {
    shape(c, (Rect){280, 136, 936, 456}, 22, PANEL);
    if (view->exit_detail) {
        text(c, 319, 218, 816, TITLE, view->returning_to_launcher ? "Returning to IZ2S…" : "Closing IZtwiNS2…", INK);
        text(c, 320, 293, 810, BODY, view->exit_detail, DIM);
    } else if (view->signing_in) {
        text(c, 319, 218, 816, TITLE, "Sign in to Twitch", INK);
        if (view->device_code && *view->device_code) {
            text(c, 320, 279, 503, BODY, "Scan the QR code with your phone.", DIM);
            text(c, 320, 313, 503, BODY, "Or open twitch.tv/activate and enter the code.", DIM);
            shape(c, (Rect){319, 348, 392, 84}, 13, CONTROL);
            text(c, 341, 407, 345, TITLE, view->device_code, ACCENT);
            qr(c, view->activation_link);
        } else text(c, 320, 291, 490, BODY, "Getting your code from Twitch…", DIM);
        if (view->cancel_pending) text(c, 319, 539, 650, BODY, "Canceling sign-in…", DIM);
        else modal_button(c, 319, "Back", GLYPH_LEFT, KEY_BACK);
    } else if (view->confirm_remember) {
        text(c, 319, 218, 816, TITLE, "Remember your Twitch session?", INK);
        wrapped(c, 320, 293, 810, BODY, "Save a Twitch session on this SD card to sign in automatically next time.", DIM, 2, 28);
        int next = modal_button(c, 319, "Remember", GLYPH_CHECK, KEY_SELECT);
        modal_button(c, next, "This session only", GLYPH_NONE, KEY_BACK);
    } else if (view->confirm_sign_out) {
        text(c, 319, 218, 816, TITLE, "Sign out of Twitch?", INK);
        wrapped(c, 320, 293, 810, BODY, "Sign in again to see followed channels or send chat messages.", DIM, 2, 28);
        int next = modal_button(c, 319, "Sign out", GLYPH_NONE, KEY_SELECT);
        modal_button(c, next, "Back", GLYPH_LEFT, KEY_BACK);
    } else {
        text(c, 319, 218, 816, TITLE, "Could not play video", INK);
        text(c, 320, 293, 810, CARD, view->playback_title, ACCENT);
        wrapped(c, 320, 344, 810, BODY, view->status, DIM, 3, 28);
        int next = modal_button(c, 319, "Retry", GLYPH_NONE, KEY_SELECT);
        modal_button(c, next, "Back", GLYPH_LEFT, KEY_BACK);
    }
}

static void footer(Canvas c, const MenuFrame *view) {
    shape(c, (Rect){CONTENT_X, 660, CONTENT_WIDTH, 1}, 0, LINE);
    const char *status = view->loading ? "Updating Twitch…" : view->status;
    if (view->list->count && status && *status)
        text(c, CONTENT_X + 2, 654, CONTENT_WIDTH - 4, SMALL, status, ACCENT);
    if (view->previous_page || view->next_page) {
        bool paging = view->focus == MENU_PAGER;
        const char *labels[] = {"Previous page", "Next page"};
        const bool available[] = {view->previous_page, view->next_page};
        for (unsigned i = 0; i < 2; ++i) {
            if (!available[i]) continue;
            bool selected = paging && view->pager_choice == i;
            int left = CONTENT_RIGHT - 280 + (int)i * 144;
            Rect area = {left, 665, 136, 40};
            hit(c, area, HIT_MENU_PAGE, i);
            shape(c, area, 10, selected ? ACCENT : PANEL);
            centered(c, left + 6, 124, 690, UI_LABEL, labels[i], selected ? ACTION_INK : INK);
        }
        if (paging) {
            int x = shortcut_hit(c, CONTENT_X, 672, "A", "Change page", HIT_KEY, KEY_SELECT);
            shortcut_hit(c, x, 672, "Up", "Back to content", HIT_KEY, KEY_UP);
            return;
        }
        c.width = CONTENT_RIGHT - 296;
        unsigned first = menu_page(view->tab, view->selected);
        unsigned size = menu_page_size(view->tab, view->selected);
        unsigned visible = view->list->count - first;
        if (visible > size) visible = size;
        unsigned local = view->selected - first;
        if (view->focus == MENU_GRID && visible && first + visible == view->list->count &&
            local / PREVIEW_COLUMNS == (visible - 1) / PREVIEW_COLUMNS) {
            const int hint = CONTENT_RIGHT - 431;
            if (!view->touch_input) shortcut_hit(c, hint, 672, "Down", "Pages", HIT_KEY, KEY_DOWN);
            c.width = hint - 12;
        }
    }
    int x = CONTENT_X;
    if (view->focus == MENU_RAIL) {
        x = shortcut_hit(c, x, 672, "A", "Open section", HIT_KEY, KEY_SELECT);
        shortcut_hit(c, x, 672, "B", "Back to content", HIT_KEY, KEY_BACK);
        return;
    }
    if (view->focus == MENU_TOOLBAR || view->focus == MENU_FILTERS) {
        x = shortcut_hit(c, x, 672, "A", "Select", HIT_KEY, KEY_SELECT);
        shortcut_hit(c, x, 672, "B", "Back to content", HIT_KEY, KEY_BACK);
        return;
    }
    const Channel *row = view->list->count ? &view->list->items[view->selected] : NULL;
    if (row) {
        if (*row->login && !menu_profile_tab(view->tab))
            x = shortcut_hit(c, x, 672, "ZL", "Channel", HIT_KEY, KEY_CHANNEL);
        if (view->tab == CONTINUE_WATCHING)
            x = shortcut_hit(c, x, 672, "−", "Remove", HIT_KEY, KEY_SAVE);
        else if (*row->login && (row->kind == ITEM_LIVE || row->kind == ITEM_CHANNEL))
            x = shortcut_hit(c, x, 672, "−", "Favorite", HIT_KEY, KEY_SAVE);
    }
    if (menu_profile_tab(view->tab)) x = shortcut(c, x, 672, "L/R", "Channel tabs");
    if (view->tab == SEARCH) x = shortcut(c, x, 672, "L/R", "Filters");
    x = shortcut_hit(c, x, 672, "B", view->tab >= SEARCH ? "Back" : "Exit", HIT_KEY, KEY_BACK);
    if (view->tab != CONTINUE_WATCHING && (view->tab != FOLLOWING || *view->login))
        shortcut_hit(c, x, 672, "+", "Refresh", HIT_KEY, KEY_REFRESH);
}

void render_menu(uint32_t *pixels, unsigned stride, const MenuFrame *view) {
    Canvas c = {pixels, stride, DISPLAY_WIDTH, view->touch};
    if (c.touch) c.touch->count = 0;
    shell(c, view);
    if (view->exit_detail || view->signing_in || view->confirm_sign_out || view->confirm_remember || view->playback_error) {
        if (c.touch) c.touch->count = 0;
        dialog(c, view);
    } else {
        grid(c, view);
        footer(c, view);
    }
}

static void centered(Canvas c, int left, int width, int baseline, Font face, const char *value, uint32_t color) {
    int length = text_width(face, value);
    int inset = length < width ? (width - length) / 2 : 0;
    text(c, left + inset, baseline, width - inset, face, value, color);
}

static void right_aligned(Canvas c, int right, int width, int baseline, Font face,
                          const char *value, uint32_t color) {
    int length = text_width(face, value);
    if (length > width) length = width;
    text(c, right - length, baseline, length, face, value, color);
}

static void rounded_border(Canvas c, Rect r, int radius, int stroke, uint32_t edge, uint32_t background) {
    r = pixel_rect(r);
    radius = ui_px(radius);
    stroke = ui_px(stroke);
    const int left = r.x < 0 ? 0 : r.x, top = r.y < 0 ? 0 : r.y;
    const int width = ui_px(c.width);
    const int right = r.x + r.w < width ? r.x + r.w : width;
    const int bottom = r.y + r.h < UI_HEIGHT ? r.y + r.h : UI_HEIGHT;
    const int inner_radius = radius > stroke ? radius - stroke : 0;
    for (int y = top; y < bottom; ++y) {
        uint32_t *row = c.pixels + y * c.stride;
        int ry = y - r.y;
        for (int x = left; x < right; ++x) {
            int rx = x - r.x;
            unsigned outer = coverage(rx, ry, r.w, r.h, radius);
            if (!outer) continue;
            unsigned inner = coverage(rx - stroke, ry - stroke, r.w - 2 * stroke, r.h - 2 * stroke, inner_radius);
            if (inner > outer) inner = outer;
            if (inner >= 255) row[x] = background;
            else {
                uint32_t paint = inner ? mix(edge, background, (inner * 255 + outer / 2) / outer) : edge;
                row[x] = outer == 255 ? paint : mix(row[x], paint, outer);
            }
        }
    }
}

enum { CHAT_TOP = 101, CHAT_BOTTOM = 603, CHAT_INSET = 20, CHAT_TEXT_WIDTH = PLAYER_CHAT_WIDTH - CHAT_INSET * 2 - 4 };
typedef struct { uint16_t start, length; } ChatLine;
typedef struct {
    ChatLine lines[CHAT_TEXT_BYTES];
    unsigned count;
} ChatMessageLayout;

/* Chat navigation and rendering both run serially on the UI thread. */
static ChatMessageLayout chat_layout;

static Font chat_body_font(TextSize size) {
    return size == TEXT_NORMAL ? SMALL : size == TEXT_LARGE ? BODY : CARD;
}

static void chat_wrap(const char *text, Font face, ChatMessageLayout *message) {
    const char *next = text;
    size_t remaining = strlen(text);
    message->count = 0;
    while (remaining && message->count < CHAT_TEXT_BYTES) {
        char fragment[193];
        size_t bytes = remaining < sizeof(fragment) ? remaining : sizeof(fragment) - 1;
        while (bytes < remaining && bytes && ((unsigned char)next[bytes] & 0xc0) == 0x80) --bytes;
        if (!bytes) break;
        memcpy(fragment, next, bytes);
        fragment[bytes] = 0;
        size_t length = typography_break(face, fragment, CHAT_TEXT_WIDTH);
        if (!length || length > bytes) length = bytes;
        size_t visible = length;
        while (visible && next[visible - 1] == ' ') --visible;
        if (visible) message->lines[message->count++] = (ChatLine){(uint16_t)(next - text), (uint16_t)visible};
        next += length;
        remaining -= length;
        while (remaining && *next == ' ') { ++next; --remaining; }
    }
    if (!message->count) message->lines[message->count++] = (ChatLine){0, 0};
}

static const ChatMessage *chat_row(const ChatSnapshot *chat, uint64_t end) {
    return &chat->messages[end - (chat->next - chat->count) - 1];
}

static ChatScroll chat_scroll_resolve(const ChatSnapshot *chat, ChatScroll position) {
    if (!chat->count) return (ChatScroll){0};
    if (!position.end || position.end > chat->next) return (ChatScroll){.end = chat->next};
    uint64_t first = chat->next - chat->count + 1;
    if (position.end < first) position.end = first;
    return position;
}

ChatScroll chat_scroll_by_lines(const ChatSnapshot *chat, ChatScroll position, TextSize size, int direction,
                              unsigned lines) {
    if (!chat->count || !direction || !lines) return position;
    position = chat_scroll_resolve(chat, position);
    uint64_t first = chat->next - chat->count + 1;
    Font face = chat_body_font(size);
    ChatMessageLayout *block = &chat_layout;
    chat_wrap(chat_row(chat, position.end)->text, face, block);
    if (position.hidden_lines >= block->count) position.hidden_lines = block->count - 1;
    for (unsigned i = 0; i < lines; ++i) {
        if (direction < 0) {
            if (position.hidden_lines + 1 < block->count) ++position.hidden_lines;
            else if (position.end > first) {
                --position.end;
                position.hidden_lines = 0;
                chat_wrap(chat_row(chat, position.end)->text, face, block);
            } else break;
        } else if (position.hidden_lines) --position.hidden_lines;
        else if (position.end < chat->next) {
            ++position.end;
            chat_wrap(chat_row(chat, position.end)->text, face, block);
            position.hidden_lines = block->count - 1;
        } else return (ChatScroll){0};
    }
    if (position.end == chat->next && !position.hidden_lines) return (ChatScroll){0};
    return position;
}

static void chat_draw_messages(Canvas c, const ChatSnapshot *chat, ChatScroll position, TextSize size) {
    if (!position.end) return;
    const Font face = chat_body_font(size);
    const int line_height = chat_line_height(size);
    const uint64_t first = chat->next - chat->count + 1;
    int bottom = CHAT_BOTTOM;
    ChatMessageLayout *block = &chat_layout;
    for (uint64_t end = position.end; end >= first && bottom > CHAT_TOP; --end) {
        const ChatMessage *row = chat_row(chat, end);
        chat_wrap(row->text, face, block);
        unsigned hidden = end == position.end ? position.hidden_lines : 0;
        unsigned lines = hidden < block->count ? block->count - hidden : 1;
        int top = bottom - (int)lines * line_height - 31;
        int clipped = top < CHAT_TOP ? CHAT_TOP : top;
        shape(c, (Rect){12, clipped, c.width - 24, bottom - clipped}, 10, RGB(24,25,36));
        uint32_t color = row->color ? RGB((row->color >> 16) & 255, (row->color >> 8) & 255, row->color & 255) : ACCENT;
        if (top >= CHAT_TOP) text(c, CHAT_INSET, top + 21, CHAT_TEXT_WIDTH, UI_LABEL, row->author, mix(color, INK, 121));
        int baseline = top + 26;
        for (unsigned line = 0; line < lines; ++line) {
            baseline += line_height;
            if (baseline - line_height < CHAT_TOP) continue;
            ChatLine part = block->lines[line];
            char segment[193];
            memcpy(segment, row->text + part.start, part.length);
            segment[part.length] = 0;
            text(c, CHAT_INSET, baseline - 4, CHAT_TEXT_WIDTH, face, segment, INK);
        }
        bottom = top - 6;
    }
}
void render_chat(uint32_t *pixels, const ChatSnapshot *chat, const char *channel, const char *notice,
                 const PlayerOverlay *view) {
    Canvas c = {pixels, UI_CHAT_WIDTH, PLAYER_CHAT_WIDTH, view->chat_touch};
    if (c.touch) c.touch->count = 0;
    const int x = 0, width = PLAYER_CHAT_WIDTH;
    const bool focus = view->page == PLAYER_CHAT_HISTORY;
    hit(c, (Rect){0, 0, width, 649}, HIT_CHAT_BODY, 0);
    shape(c, (Rect){x, 0, width, DISPLAY_HEIGHT}, 0, RGB(15,16,24));
    shape(c, (Rect){x, 0, 1, DISPLAY_HEIGHT}, 0, focus ? ACCENT : LINE);
    text(c, x + 24, 45, 205, CARD, "Stream chat", INK);
    char handle[CHANNEL_LOGIN_BYTES + 2];
    snprintf(handle, sizeof(handle), "@%s", channel);
    text(c, x + 24, 73, width - 48, SMALL, handle, DIM);
    bool ready = chat->state == CHAT_GUEST || chat->state == CHAT_CONNECTED;
    const char *state = chat->state == CHAT_GUEST ? "Read only" : chat->state == CHAT_CONNECTED ? "Connected" :
                        chat->state == CHAT_UNAVAILABLE ? "Offline" : "Connecting";
    const uint32_t state_ink = ready ? RGB(143,227,181) : chat->state == CHAT_UNAVAILABLE ? RGB(255,166,175) : DIM;
    int badge = text_width(UI_LABEL, state) + 35;
    int bx = width - 24 - badge;
    shape(c, (Rect){bx, 26, badge, 27}, 13, CONTROL);
    shape(c, (Rect){bx + 10, 36, 7, 7}, 4, state_ink);
    text(c, bx + 24, 45, badge - 31, UI_LABEL, state, state_ink);
    shape(c, (Rect){x + 24, 90, width - 48, 1}, 0, focus ? ACCENT : LINE);

    const ChatScroll position = chat_scroll_resolve(chat, view->chat_scroll);
    chat_draw_messages(c, chat, position, view->chat_size);
    if (!chat->count) {
        const char *placeholder = ready ? "Waiting for messages…" :
                                  chat->state == CHAT_UNAVAILABLE ? "Chat is unavailable" : "Joining the conversation…";
        centered(c, x + 24, width - 48, 289, BODY, placeholder, DIM);
    }
    uint64_t unread = chat->next - position.end;
    bool scrolled = position.hidden_lines || unread;
    const char *detail = chat->sending ? "Sending message…" : *chat->delivery ? chat->delivery :
                         *notice ? notice : chat->state == CHAT_UNAVAILABLE ? chat->error : "";
    if (*detail) wrapped(c, x + 24, 619, width - 48, SMALL, detail, ACCENT, 2, 17);
    else if (unread) {
        char count[45];
        if (unread > 99) snprintf(count, sizeof(count), "99+ new messages");
        else snprintf(count, sizeof(count), "%llu new %s", (unsigned long long)unread, unread == 1 ? "message" : "messages");
        text(c, x + 24, 635, width - 48, SMALL, count, ACCENT);
    } else if (!focus && ready)
        text(c, x + 24, 635, width - 48, SMALL,
             view->touch_input ? "Swipe to read chat history" : "Right from Settings to read chat", DIM);
    shape(c, (Rect){x + 18, 654, width - 36, 43}, 11, focus ? FOCUS : PANEL);
    if (focus) {
        int next = x + 28;
        if (!view->touch_input) next = shortcut(c, next, 663, "↑ ↓", "Scroll");
        if (scrolled) next = shortcut_hit(c, next, 663, "A", "Live", HIT_CHAT_LIVE, 0);
        shortcut_hit(c, next, 663, "B", "Back", HIT_CHAT_BACK, 0);
        return;
    }
    const char *hint = chat->sending ? "Waiting for Twitch" :
        chat->can_send ? (*chat->delivery ? "Edit message draft" : "Write a message") :
        chat->state == CHAT_GUEST ? "Sign in to send messages" :
        chat->state == CHAT_UNAVAILABLE ? "Select Chat to retry" : "Twitch chat";
    text(c, x + 32, 681, width - 64, SMALL, hint, DIM);
    if (chat->can_send)
        hit(c, (Rect){x + 18, 654, width - 36, 43}, HIT_CHAT_MESSAGE, 0);
}

static void quality_name(char out[40], unsigned height, unsigned fps) {
    if (!height) snprintf(out, 40, "Unavailable");
    else if (fps) snprintf(out, 40, "%up  ·  %u fps", height, fps);
    else snprintf(out, 40, "%up", height);
}

static void playback_time(char out[24], unsigned ms) {
    unsigned sec = ms / 1000;
    if (sec >= 3600) snprintf(out, 24, "%u:%02u:%02u", sec / 3600, sec / 60 % 60, sec % 60);
    else snprintf(out, 24, "%u:%02u", sec / 60, sec % 60);
}

unsigned player_actions(ItemKind kind, bool chat, PlayerAction out[PLAYER_ACTIONS]) {
    unsigned count = 0;
    out[count++] = PLAYER_PLAY;
    if (kind == ITEM_VIDEO) {
        out[count++] = PLAYER_BACK;
        out[count++] = PLAYER_FORWARD;
    }
    out[count++] = PLAYER_SOUND;
    if (kind == ITEM_LIVE) {
        out[count++] = PLAYER_CHAT;
        if (chat) out[count++] = PLAYER_MESSAGE;
    }
    out[count++] = PLAYER_SETTINGS;
    return count;
}

static const char *action_name(PlayerAction action, const PlayerOverlay *view) {
    switch (action) {
        case PLAYER_PLAY: return view->paused ? "Resume" : "Pause";
        case PLAYER_BACK: return "Back 30s";
        case PLAYER_FORWARD: return "Forward 30s";
        case PLAYER_SOUND: return view->muted ? "Unmute" : "Mute";
        case PLAYER_CHAT: return view->chat_failed ? "Retry chat" : view->chat ? "Hide chat" : "Chat";
        case PLAYER_MESSAGE: return "Message";
        case PLAYER_SETTINGS: return "Settings";
        case PLAYER_ACTIONS: break;
    }
    return "";
}

static Glyph action_icon(PlayerAction action, const PlayerOverlay *view) {
    switch (action) {
        case PLAYER_PLAY: return view->paused ? GLYPH_PLAY : GLYPH_PAUSE;
        case PLAYER_BACK: return GLYPH_BACK;
        case PLAYER_FORWARD: return GLYPH_FORWARD;
        case PLAYER_SOUND: return view->muted ? GLYPH_MUTE : GLYPH_VOLUME;
        case PLAYER_CHAT: return GLYPH_CHAT;
        case PLAYER_MESSAGE: return GLYPH_MESSAGE;
        case PLAYER_SETTINGS: return GLYPH_SETTINGS;
        case PLAYER_ACTIONS: return GLYPH_NONE;
    }
    return GLYPH_NONE;
}

static void shade_row(Canvas c, int y, int width, unsigned dark, bool layer) {
    uint32_t *row = c.pixels + y * c.stride;
    if (layer) {
        for (int x = 0; x < width; ++x) row[x] = (uint32_t)dark << 24;
        return;
    }
    unsigned light = 256 - dark;
    for (int x = 0; x < width; ++x) {
        uint32_t p = row[x];
        uint32_t rb = (((p & 0x00ff00ffu) * light) >> 8) & 0x00ff00ffu;
        uint32_t g = (((p & 0x0000ff00u) * light) >> 8) & 0x0000ff00u;
        row[x] = 0xff000000u | rb | g;
    }
}

static void scrims(Canvas c, int width, bool layer) {
    width = ui_px(width);
    for (int y = 0; y < ui_px(172); ++y) {
        unsigned dark = y < ui_px(102) ? 226 - (unsigned)y * 62 / ui_px(102)
                                       : (unsigned)(ui_px(172) - y) * 164 / ui_px(70);
        shade_row(c, y, width, dark, layer);
    }
    for (int y = ui_px(488); y < UI_HEIGHT; ++y) {
        unsigned dark = y < ui_px(627) ? (unsigned)(y - ui_px(488)) * 225 / ui_px(139)
                                       : 225 + (unsigned)(y - ui_px(627)) * 21 / ui_px(93);
        shade_row(c, y, width, dark, layer);
    }
}

static void player_header(Canvas c, const PlayerOverlay *view, int width) {
    const bool live = view->kind == ITEM_LIVE;
    const int badge = live ? 74 : view->kind == ITEM_CLIP ? 62 : 70;
    uint32_t background = live ? RGB(141,32,63) : RGB(36,34,51);
    shape(c, (Rect){40, 32, badge, 27}, 13, background);
    if (live) shape(c, (Rect){51, 42, 7, 7}, 4, RGB(255,149,173));
    const char *type = live ? "LIVE" : view->kind == ITEM_CLIP ? "CLIP" : "VIDEO";
    text(c, live ? 65 : 52, 51, badge - (live ? 27 : 18), UI_LABEL, type, INK);
    if (view->channel && *view->channel) text(c, 52 + badge, 51, width - 360, LABEL, view->channel, INK);
    bool compact_title = view->page == PLAYER_QUALITY_MENU && view->count >= 7;
    int title_width = width - (compact_title ? 460 : 80);
    text(c, 40, 107, title_width, TITLE, view->title, INK);
    const bool cancel = view->buffering || view->page == PLAYER_TIMELINE;
    int left = right_shortcut(c, width - 40, 33, "B", cancel ? "Cancel" :
                              view->page == PLAYER_CHAT_HISTORY ? "Player controls" : "Back", KEY_BACK);
    if (!view->buffering && view->page != PLAYER_CHAT_HISTORY)
        right_shortcut(c, left, 33, "A", view->page == PLAYER_TIMELINE ? "Seek" : "Select", KEY_SELECT);
}

static void player_captions(Canvas c, const PlayerOverlay *view, int width, bool chrome) {
    enum { max_lines = 5 };
    char copy[CAPTION_BYTES], fragments[CAPTION_BYTES + max_lines];
    snprintf(copy, sizeof(copy), "%s", view->caption);
    const char *lines[max_lines];
    char *fragment = fragments;
    int count = 0, widest = 0;
    Font face = view->captions_size == TEXT_NORMAL ? CARD :
                view->captions_size == TEXT_LARGE ? CAPTION_LARGE : CAPTION_XL;
    int line_gap = 30 + (int)view->captions_size * 7;
    for (char *line = copy; line && count < max_lines; ) {
        char *end = strchr(line, '\n');
        if (end) *end++ = 0;
        while (*line && count < max_lines) {
            while (*line == ' ') ++line;
            if (!*line) break;
            // Leave overflow on the last row for typography_draw to elide.
            size_t bytes = count + 1 == max_lines ? strlen(line) : typography_break(face, line, width - 84);
            if (!bytes) break;
            memcpy(fragment, line, bytes);
            fragment[bytes] = 0;
            lines[count++] = fragment;
            int length = text_width(face, fragment);
            if (length > widest) widest = length;
            fragment += bytes + 1;
            line += bytes;
        }
        line = end;
    }
    if (!count) return;
    int bottom = chrome ? 539 : view->chat ? 603 : 652;
    int box_width = widest + 44;
    if (box_width > width - 48) box_width = width - 48;
    int left = (width - box_width) / 2;
    int top = bottom - count * line_gap - 18;
    shape(c, (Rect){left, top, box_width, bottom - top}, 10, RGB(13,13,20));
    for (int i = 0; i < count; ++i)
        centered(c, left + 18, box_width - 36, top + line_gap + 2 + i * line_gap, face, lines[i], INK);
}

static void player_progress(Canvas c, const PlayerOverlay *view, int width) {
    const int left = PLAYER_CONTROL_INSET, right = width - left, bar = right - left;
    if (!view->duration_ms) return;
    if (view->kind == ITEM_VIDEO && (view->page == PLAYER_CONTROLS || view->page == PLAYER_TIMELINE))
        hit(c, (Rect){left - 15, 568, bar + 30, 44}, HIT_TIMELINE, 0);
    bool seeking = view->page == PLAYER_TIMELINE;
    unsigned at = seeking ? view->seek_ms : view->position_ms;
    if (at > view->duration_ms) at = view->duration_ms;
    int fill = (int)((uint64_t)at * bar / view->duration_ms);
    char time[24], duration[24];
    playback_time(time, at);
    playback_time(duration, view->duration_ms);
    if (!seeking) text(c, left, 579, 100, LABEL, time, INK);
    if (!seeking || fill < bar - 196) right_aligned(c, right, 100, 579, LABEL, duration, INK);
    shape(c, (Rect){left, 595, bar, seeking ? 6 : 4}, 2, RGB(103,101,119));
    if (fill) shape(c, (Rect){left, 595, fill, seeking ? 6 : 4}, 2, ACCENT);
    int center = left + fill;
    if (seeking) {
        int bubble = center - 43;
        if (bubble < left) bubble = left;
        if (bubble > right - 86) bubble = right - 86;
        shape(c, (Rect){bubble, 555, 86, 33}, 8, ACCENT);
        centered(c, bubble + 6, 74, 578, BODY, time, RGB(27,20,47));
    }
    int radius = seeking ? 8 : 5;
    shape(c, (Rect){center - radius, 597 - radius, 2 * radius, 2 * radius}, radius, INK);
}

static void player_action_bar(Canvas c, const PlayerOverlay *view, int width) {
    PlayerAction actions[PLAYER_ACTIONS];
    unsigned count = player_actions(view->kind, view->chat, actions);
    const unsigned transport = view->kind == ITEM_VIDEO ? 3 : 1;
    enum { left = 40, top = 614, inset = 8, gap = 6, button_height = 62 };
    const int right = width - left;
    const char *labels[PLAYER_ACTIONS];
    int button_widths[PLAYER_ACTIONS];
    int left_group = 0, right_group = 0;
    for (unsigned i = 0; i < count; ++i) {
        labels[i] = action_name(actions[i], view);
        int label_width = text_width(UI_LABEL, labels[i]) + 22;
        button_widths[i] = label_width > 74 ? label_width : 74;
        if (i < transport) left_group += button_widths[i] + gap;
        else right_group += button_widths[i] + gap;
    }
    shape(c, (Rect){left, top, right - left, button_height + 2 * inset}, 17, RGB(20,19,31));
    int x = left + inset;
    for (unsigned i = 0; i < count; ++i) {
        if (i == transport) x = right - inset - right_group + gap;
        PlayerAction action = actions[i];
        bool focused = view->page == PLAYER_CONTROLS && i == view->focus;
        bool active = (action == PLAYER_CHAT && view->chat) ||
                      (action == PLAYER_SETTINGS && (view->page == PLAYER_SETTINGS_MENU ||
                                                     view->page == PLAYER_QUALITY_MENU));
        int button_width = button_widths[i];
        hit(c, (Rect){x, top + inset, button_width, button_height}, HIT_PLAYER_ACTION, i);
        if (focused || active || action == PLAYER_PLAY)
            shape(c, (Rect){x, top + inset, button_width, button_height}, 12,
                  focused ? ACCENT : active ? FOCUS : CONTROL);
        bool unavailable = action == PLAYER_MESSAGE && !view->chat_can_send;
        uint32_t ink = focused ? ACTION_INK : active ? ACCENT : unavailable ? DIM : INK;
        glyph(c, x + button_width / 2, top + 27, action_icon(action, view), ink);
        centered(c, x, button_width, top + 57, UI_LABEL, labels[i], ink);
        x += button_width + gap;
    }
    int info_left = left + inset + left_group;
    int info_width = right - inset - right_group - info_left;
    if (view->page == PLAYER_CONTROLS && info_width > 0) {
        if (view->kind == ITEM_VIDEO && view->duration_ms)
            centered(c, info_left, info_width, top + 45, SMALL,
                     view->touch_input ? "Drag the timeline to seek" : "Up for timeline", DIM);
        else if (view->quality) {
            char quality[40];
            quality_name(quality, view->quality, view->fps);
            centered(c, info_left, info_width, top + 45, SMALL, quality, DIM);
        }
    }
}

static void settings_panel(Canvas c, const PlayerOverlay *view, int width) {
    const bool quality = view->page == PLAYER_QUALITY_MENU;
    const int rows = quality ? (int)view->count + 1 : (int)PLAYER_SETTINGS_ROWS;
    const int height = 72 + 44 * rows + (quality ? 13 : 34);
    const int left = width - 388, top = 548 - height;
    hit(c, (Rect){left, top, 348, height}, HIT_PANEL, 0);
    shape(c, (Rect){left + 3, top + 5, 348, height}, 19, BG);
    shape(c, (Rect){left, top, 348, height}, 19, PANEL);
    text(c, left + 22, top + 41, 303, CARD, quality ? "Video quality" : "Playback settings", INK);
    shape(c, (Rect){left + 20, top + 55, 308, 1}, 0, LINE);
    for (int i = 0; i < rows; ++i) {
        int y = top + 65 + i * 44;
        hit(c, (Rect){left + 10, y, 328, 40}, quality ? HIT_QUALITY : HIT_SETTING, (unsigned)i);
        bool selected = quality ? (unsigned)i == view->choice : (unsigned)i == (unsigned)view->setting;
        if (selected) shape(c, (Rect){left + 10, y, 328, 40}, 10, ACCENT);
        uint32_t ink = selected ? ACTION_INK : INK;
        uint32_t detail = selected ? ACTION_INK : DIM;
        if (quality) {
            char label[40] = "Auto";
            unsigned h = i ? view->qualities[i - 1] : 0;
            if (i) quality_name(label, h, view->rates[i - 1]);
            text(c, left + 24, y + 27, 249, BODY, label, ink);
            if (h == view->preferred)
                glyph(c, left + 309, y + 20, GLYPH_CHECK, selected ? ACTION_INK : ACCENT);
        } else if (i == SETTINGS_QUALITY) {
            char value[40];
            quality_name(value, view->quality, view->fps);
            text(c, left + 24, y + 27, 160, BODY, "Video quality", ink);
            right_aligned(c, left + 301, 134, y + 27, UI_LABEL, value, detail);
            glyph(c, left + 314, y + 20, GLYPH_RIGHT, detail);
        } else if (i == SETTINGS_CAPTIONS) {
            text(c, left + 24, y + 27, 220, BODY, "Closed captions", ink);
            uint32_t track = view->captions_on ? (selected ? ACTION_INK : FOCUS) :
                             (selected ? RGB(155,126,212) : LINE);
            shape(c, (Rect){left + 281, y + 9, 41, 22}, 11, track);
            shape(c, (Rect){left + (view->captions_on ? 302 : 284), y + 12, 16, 16}, 8, INK);
        } else {
            static const char *names[] = {"Default", "Large", "Extra large"};
            const bool captions = i == SETTINGS_CAPTIONS_TEXT;
            TextSize size = captions ? view->captions_size : view->chat_size;
            text(c, left + 24, y + 27, 188, BODY, captions ? "Caption text size" : "Chat text size", ink);
            right_aligned(c, left + 322, 108, y + 27, UI_LABEL, names[size], detail);
        }
    }
    if (!quality && view->captions_on) {
        char track[96];
        if (view->caption_track && *view->caption_track)
            snprintf(track, sizeof(track), "%s · WebVTT", view->caption_track);
        else snprintf(track, sizeof(track), "%s", view->caption_seen ? "Embedded captions" : "Waiting for a caption track");
        text(c, left + 24, top + height - 14, 300, SMALL, track, DIM);
    }
}

static void player_loading(Canvas c, const PlayerOverlay *view, int width) {
    int middle = width / 2, x = middle - 172, y = 310;
    shape(c, (Rect){x, y, 344, 91}, 19, PANEL);
    const int offsets[8][2] = {{0,-13},{9,-9},{13,0},{9,9},{0,13},{-9,9},{-13,0},{-9,-9}};
    for (unsigned i = 0; i < 8; ++i) {
        unsigned age = (view->busy_phase + 8 - i) % 8;
        uint32_t ink = mix(FOCUS, ACCENT, 255 - 29 * age);
        shape(c, (Rect){x + 39 + offsets[i][0] - 3, y + 45 + offsets[i][1] - 3, 6, 6}, 3, ink);
    }
    text(c, x + 68, y + 52, 259, BODY, view->buffering, INK);
}

static void render_overlay(uint32_t *pixels, unsigned stride, const PlayerOverlay *view, bool layer) {
    int width = view->chat ? PLAYER_CHAT_EDGE : DISPLAY_WIDTH;
    Canvas c = {pixels, stride, layer ? width : DISPLAY_WIDTH, view->touch};
    if (c.touch) c.touch->count = 0;
    bool chrome = view->chrome || view->paused || view->page != PLAYER_CONTROLS || view->buffering;
    bool settings = view->page == PLAYER_SETTINGS_MENU || view->page == PLAYER_QUALITY_MENU;
    if (chrome) {
        scrims(c, width, layer);
        player_header(c, view, width);
    }
    if (view->buffering) {
        player_loading(c, view, width);
        if (!view->stalled) return;
    }
    if (view->captions_on && view->caption && *view->caption)
        player_captions(c, view, settings ? width - 400 : width, chrome);
    if (!chrome) return;
    player_progress(c, view, width);
    player_action_bar(c, view, width);
    if (settings) settings_panel(c, view, width);
}

void render_player_overlay(uint32_t *pixels, unsigned stride, const PlayerOverlay *view) {
    render_overlay(pixels, stride, view, false);
}

#ifdef IZTWINS2_IZA
void render_player_layer(uint32_t *pixels, unsigned stride, const PlayerOverlay *view) {
    render_overlay(pixels, stride, view, true);
}
#endif
