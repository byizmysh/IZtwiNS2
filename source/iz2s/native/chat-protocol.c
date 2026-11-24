/* SPDX-License-Identifier: GPL-3.0-only */
#include "chat-protocol.h"
#include "twitch.h"
#include <stdio.h>
#include <string.h>

static void copy_visible(char *out, size_t cap, const char *value, size_t size) {
    size_t count = size < cap ? size : cap - 1;
    while (count && count < size && ((unsigned char)value[count] & 0xc0) == 0x80) --count;
    for (size_t i = 0; i < count; ++i) out[i] = (unsigned char)value[i] < 32 ? ' ' : value[i];
    out[count] = 0;
}

static const char *tag(const char *begin, const char *end, const char *key, size_t *size) {
    size_t name = strlen(key);
    while (begin < end) {
        const char *next = memchr(begin, ';', (size_t)(end - begin));
        if (!next) next = end;
        if ((size_t)(next - begin) > name && begin[name] == '=' && !memcmp(begin, key, name)) {
            *size = (size_t)(next - begin) - name - 1;
            return begin + name + 1;
        }
        if (next == end) break;
        begin = next + 1;
    }
    return NULL;
}

static int hex(unsigned char ch) {
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= '0' && ch <= '9') return ch - '0';
    return -1;
}

bool chat_parse(const char *line, const char *room, ChatMessage *out) {
    const char *begin = NULL, *end = NULL;
    if (*line == '@') {
        begin = line + 1;
        end = strchr(begin, ' ');
        if (!end) return false;
        line = end + 1;
    }
    if (*line++ != ':') return false;
    const char *prefix = strchr(line, ' '), *bang = strchr(line, '!');
    if (!prefix) return false;
    if (!bang || bang > prefix) bang = prefix;
    const char marker[] = " PRIVMSG #";
    if (strncmp(prefix, marker, sizeof(marker) - 1)) return false;
    const char *channel = prefix + sizeof(marker) - 1;
    size_t length = strlen(room);
    if (strncmp(channel, room, length) || strncmp(channel + length, " :", 2)) return false;
    const char *message = channel + length + 2;
    *out = (ChatMessage){0};
    copy_visible(out->text, sizeof(out->text), message, strlen(message));
    copy_visible(out->author, sizeof(out->author), line, (size_t)(bang - line));
    if (begin) {
        size_t size = 0;
        const char *id = tag(begin, end, "id", &size);
        if (id && size < sizeof(out->id)) copy_visible(out->id, sizeof(out->id), id, size);
        const char *display = tag(begin, end, "display-name", &size);
        if (display && size) copy_visible(out->author, sizeof(out->author), display, size);
        const char *color = tag(begin, end, "color", &size);
        if (color && size == 7 && *color == '#') {
            uint32_t value = 0;
            for (unsigned i = 0; i < 6; ++i) {
                int digit = hex((unsigned char)color[i + 1]);
                if (digit < 0) { value = 0; break; }
                value = value * 16 + (unsigned)digit;
            }
            out->color = value;
        }
    }
    return *out->text && *out->author;
}

bool chat_message_valid(const char *message) {
    if (!*message || strlen(message) >= CHAT_TEXT_BYTES) return false;
    unsigned characters = 0;
    for (const unsigned char *p = (const unsigned char *)message; *p; ++p) {
        if (*p < 32 || *p == 127) return false;
        if ((*p & 0xc0) != 0x80 && ++characters > CHAT_MAX_CHARACTERS) return false;
    }
    return characters > 0;
}
