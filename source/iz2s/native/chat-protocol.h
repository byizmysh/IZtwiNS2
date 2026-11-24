/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef IZTWINS2_CHAT_PROTOCOL_H
#define IZTWINS2_CHAT_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { CHAT_MAX_CHARACTERS = 500, CHAT_TEXT_BYTES = CHAT_MAX_CHARACTERS * 4 + 1, CHAT_LINES = 80 };
typedef struct { char author[80], text[CHAT_TEXT_BYTES]; uint32_t color; char id[48]; } ChatMessage;
typedef enum { CHAT_IDLE, CHAT_CONNECTING, CHAT_CONNECTED, CHAT_GUEST, CHAT_UNAVAILABLE } ChatState;
typedef struct {
    ChatMessage messages[CHAT_LINES];
    unsigned count;
    uint64_t next;
    ChatState state;
    bool can_send, sending;
    char error[128], delivery[128];
} ChatSnapshot;

bool chat_parse(const char *line, const char *room, ChatMessage *out);
bool chat_message_valid(const char *message);
#endif
