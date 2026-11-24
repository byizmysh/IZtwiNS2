/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef IZTWINS2_CHAT_H
#define IZTWINS2_CHAT_H
#include "account-types.h"
#include "chat-protocol.h"
#include <stdatomic.h>
#include <switch.h>

typedef struct {
    Thread thread;
    Mutex lock;
    atomic_bool stop, finished;
    bool running, authenticated, can_write, sending;
    unsigned revision, count;
    uint64_t next;
    ChatState state;
    char room[CHANNEL_LOGIN_BYTES], login[CHANNEL_LOGIN_BYTES], access[TWITCH_TOKEN_BYTES];
    char broadcaster[32], sender[32], draft[CHAT_TEXT_BYTES];
    char error[128], delivery[128];
    ChatMessage messages[CHAT_LINES];
} TwitchChat;

bool chat_start(TwitchChat *, const char *room, const TwitchAccount *);
void chat_stop(TwitchChat *);
void chat_request_stop(TwitchChat *);
bool chat_reap(TwitchChat *);
bool chat_queue(TwitchChat *, const char *message);
void chat_draft(TwitchChat *, char out[CHAT_TEXT_BYTES]);
bool chat_snapshot(TwitchChat *, ChatSnapshot *, unsigned *revision);
#endif
