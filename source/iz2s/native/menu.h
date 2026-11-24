/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef IZTWINS2_MENU_H
#define IZTWINS2_MENU_H

#include "account-types.h"
#include "chat-protocol.h"
#include "preview.h"
#include "twitch.h"
#include "ui-size.h"
#include "touch.h"

typedef enum { FOLLOWING, POPULAR, FAVORITES, RECENT, TRENDS, CONTINUE_WATCHING, SEARCH, CATEGORY,
               CHANNEL_LIVE, CHANNEL_VIDEOS, CHANNEL_CLIPS, MENU_TAB_COUNT } MenuTab;
typedef enum { MENU_GRID, MENU_RAIL, MENU_TOOLBAR, MENU_FILTERS, MENU_PAGER } MenuFocus;
typedef enum { TOOL_SEARCH, TOOL_LINK, TOOL_ACCOUNT, MENU_TOOLS } MenuTool;
typedef enum { SEARCH_ALL, SEARCH_LIVE, SEARCH_CHANNELS, SEARCH_CATEGORIES, SEARCH_VIDEOS, SEARCH_FILTERS } SearchFilter;
enum { MENU_ROOT_TABS = 6, HOME_FIRST_ITEMS = 4 };
enum { PLAYER_CONTROL_INSET = 40 };
extern const MenuTab menu_root_tabs[MENU_ROOT_TABS];
static inline unsigned menu_rail_step(unsigned index, bool previous) {
    return (index + (previous ? MENU_ROOT_TABS - 1 : 1)) % MENU_ROOT_TABS;
}

typedef struct {
    MenuTab tab, root_tab;
    MenuFocus focus;
    TouchLayout *touch;
    unsigned rail, tool, selector, selector_hover, page_number, pager_choice;
    const ChannelList *list;
    unsigned selected;
    const char *login, *status, *device_code, *activation_link, *playback_title;
    const char *heading, *channel_note, *query, *clip_filter;
    const char *exit_detail;
    const PreviewImage *previews[PREVIEW_VISIBLE];
    unsigned progress[PREVIEW_VISIBLE], duration[PREVIEW_VISIBLE];
    bool saved[PREVIEW_VISIBLE];
    bool loading, signing_in, confirm_sign_out, confirm_remember, restoring_session, cancel_pending, playback_error;
    bool previous_page, next_page;
    bool returning_to_launcher, touch_input;
} MenuFrame;

typedef enum { PLAYER_PLAY, PLAYER_BACK, PLAYER_FORWARD, PLAYER_SOUND, PLAYER_CHAT,
               PLAYER_MESSAGE, PLAYER_SETTINGS, PLAYER_ACTIONS } PlayerAction;
typedef enum { PLAYER_CONTROLS, PLAYER_SETTINGS_MENU, PLAYER_QUALITY_MENU, PLAYER_TIMELINE, PLAYER_CHAT_HISTORY } PlayerPage;
typedef struct {
    uint64_t end;
    unsigned hidden_lines;
} ChatScroll;

typedef enum { SETTINGS_QUALITY, SETTINGS_CAPTIONS, SETTINGS_CAPTIONS_TEXT, SETTINGS_CHAT_TEXT, PLAYER_SETTINGS_ROWS } PlayerSetting;

typedef struct {
    const char *title, *channel, *caption, *caption_track, *buffering;
    const unsigned *qualities, *rates;
    TouchLayout *touch, *chat_touch;
    unsigned count, choice, preferred, quality, fps, position_ms, duration_ms, seek_ms, focus, busy_phase;
    ItemKind kind;
    PlayerPage page;
    PlayerSetting setting;
    TextSize captions_size, chat_size;
    ChatScroll chat_scroll;
    bool chrome, paused, captions_on, caption_seen, muted, chat, chat_failed, chat_can_send, stalled, touch_input;
} PlayerOverlay;

unsigned player_actions(ItemKind, bool chat, PlayerAction out[PLAYER_ACTIONS]);
static inline bool menu_profile_tab(MenuTab tab) { return tab >= CHANNEL_LIVE && tab <= CHANNEL_CLIPS; }
static inline bool menu_root_tab(MenuTab tab) { return tab <= CONTINUE_WATCHING; }
static inline unsigned menu_page_size(MenuTab tab, unsigned selection) {
    if (tab == POPULAR && selection < HOME_FIRST_ITEMS) return HOME_FIRST_ITEMS;
    return PREVIEW_VISIBLE;
}
static inline unsigned menu_page(MenuTab tab, unsigned selection) {
    if (tab == POPULAR) {
        if (selection < HOME_FIRST_ITEMS) return 0;
        return 1 + ((selection - 1) / PREVIEW_COLUMNS - 1) * PREVIEW_COLUMNS;
    }
    return selection / PREVIEW_VISIBLE * PREVIEW_VISIBLE;
}
void render_menu(uint32_t *pixels, unsigned stride, const MenuFrame *);
static inline int chat_line_height(TextSize size) { return 20 + 4 * (int)size; }
ChatScroll chat_scroll_by_lines(const ChatSnapshot *, ChatScroll, TextSize, int direction, unsigned lines);
void render_chat(uint32_t *pixels, const ChatSnapshot *, const char *channel, const char *notice,
                 const PlayerOverlay *);
void render_player_overlay(uint32_t *pixels, unsigned stride, const PlayerOverlay *);
#ifdef IZTWINS2_IZA
void render_player_layer(uint32_t *pixels, unsigned stride, const PlayerOverlay *);
#endif

#endif
