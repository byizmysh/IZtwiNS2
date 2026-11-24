/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef IZTWINS2_LIBRARY_H
#define IZTWINS2_LIBRARY_H

#include "account-types.h"

typedef struct {
    ChannelList favorites, recent, resume;
    TextSize captions_size, chat_size;
    unsigned preferred_quality;
    bool captions_on;
} ChannelLibrary;

bool library_contains(const ChannelList *, const char *login);
bool library_toggle_favorite(ChannelLibrary *, const Channel *);
void library_add_recent(ChannelLibrary *, const char *login);
const Channel *library_resume(const ChannelLibrary *, const char *video_id);
bool library_remove_resume(ChannelLibrary *, const char *video_id);
bool library_set_resume(ChannelLibrary *, const Channel *, unsigned position_ms, unsigned duration_ms);
bool library_load(ChannelLibrary *, const char *path);
bool library_save(const ChannelLibrary *, const char *path);
bool session_load(TwitchAccount *, const char *path);
bool session_save(const TwitchAccount *, const char *path);
bool session_remove(const char *path);

#endif
