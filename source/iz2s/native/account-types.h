/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef IZTWINS2_ACCOUNT_TYPES_H
#define IZTWINS2_ACCOUNT_TYPES_H

#include <stdbool.h>
#include <stdint.h>

enum { CHANNEL_LOGIN_BYTES = 26, CHANNEL_ROWS = 50, TWITCH_TOKEN_BYTES = 512, THUMB_URL_BYTES = 512 };
enum { FAVORITE_QUERY_BYTES = CHANNEL_ROWS * CHANNEL_LOGIN_BYTES + 1 };
enum { CATALOG_CURSOR_BYTES = 256 };

typedef enum { ITEM_LIVE, ITEM_CHANNEL, ITEM_CATEGORY, ITEM_VIDEO, ITEM_CLIP } ItemKind;
typedef enum { FEED_POPULAR, FEED_TRENDS, FEED_CATEGORY, FEED_SEARCH, FEED_CHANNEL, FEED_FAVORITES,
               FEED_VIDEOS, FEED_CLIPS } PublicFeed;
typedef enum { TEXT_NORMAL, TEXT_LARGE, TEXT_EXTRA_LARGE, TEXT_SIZE_COUNT } TextSize;

typedef struct {
    char login[CHANNEL_LOGIN_BYTES];
    char id[96];
    char name[96];
    char title[224];
    char thumbnail[THUMB_URL_BYTES];
    unsigned viewers, duration_ms, resume_ms;
    ItemKind kind;
    bool status_known;
} Channel;

typedef struct {
    Channel items[CHANNEL_ROWS];
    unsigned count;
    char next[CATALOG_CURSOR_BYTES];
} ChannelList;

typedef struct {
    Channel header;
    char description[384];
    ChannelList videos, clips;
} ChannelProfile;

typedef struct {
    char device[TWITCH_TOKEN_BYTES];
    char code[20];
    char link[128];
    unsigned expires, interval;
} DeviceCode;

typedef struct {
    char access[TWITCH_TOKEN_BYTES], refresh[TWITCH_TOKEN_BYTES];
    char id[32], login[CHANNEL_LOGIN_BYTES];
    uint64_t expires, validate_at;
    bool irc_read, chat_write;
} TwitchAccount;

#ifdef __cplusplus
extern "C" {
#endif
bool parse_device_code(const char *, DeviceCode *);
bool parse_account_tokens(const char *, TwitchAccount *, unsigned *expires);
bool parse_account_identity(const char *, const char *client_id, TwitchAccount *, unsigned *expires);
bool parse_channel_list(const char *, bool search, ChannelList *);
bool parse_oauth_error(const char *, char out[64]);
bool parse_helix_user_id(const char *, char out[32]);
bool chat_payload(const char *broadcaster, const char *sender, const char *text, char *out, unsigned capacity);
bool parse_chat_delivery(const char *, char id[48], char error[128]);
bool directory_request(PublicFeed, const char *key, const char *cursor, char *out, unsigned capacity);
bool parse_directory(const char *, PublicFeed, ChannelList *, ChannelProfile *);
#ifdef __cplusplus
}
#endif

#endif
