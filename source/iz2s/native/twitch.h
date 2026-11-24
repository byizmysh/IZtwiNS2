#ifndef IZTWINS2_TWITCH_H
#define IZTWINS2_TWITCH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { URL_MAX = 4096, TWITCH_CHANNEL_MAX = 25, SEGMENTS_MAX = 32 };
enum { TWITCH_MAX_WIDTH = 1280, TWITCH_MAX_HEIGHT = 720, TWITCH_QUALITIES = 8 };

typedef struct {
    char url[URL_MAX];
    unsigned height;
    unsigned fps;
    char subtitle_group[64];
} TwitchQuality;

typedef struct {
    char *uri;
    unsigned at_ms;
    const char *init;
} TwitchVodSegment;

typedef struct {
    uint64_t first;
    unsigned count;
    bool ended;
    char *uri[SEGMENTS_MAX];
    const char *init[SEGMENTS_MAX];
} TwitchPlaylist;

bool twitch_channel(char *channel);
bool twitch_token_request(const char *channel, char *out, size_t capacity);
bool twitch_master_url(const char *channel, const char *json, char out[URL_MAX]);
bool twitch_video_id(const char *id);
bool twitch_clip_id(const char *id);
bool twitch_vod_request(const char *video_id, char *out, size_t capacity);
bool twitch_vod_url(const char *video_id, const char *json, char out[URL_MAX]);
bool twitch_clip_request(const char *slug, char *out, size_t capacity);
bool twitch_sign_clip(const char *source, const char *token, const char *signature, char out[URL_MAX]);
bool twitch_split_https(const char *url, char host[128], const char **path);
bool twitch_join_url(const char *base, const char *uri, char out[URL_MAX]);
bool twitch_attribute(const char *attributes, const char *key, char *out, size_t capacity);
unsigned twitch_qualities(char *playlist, const char *url, TwitchQuality out[TWITCH_QUALITIES]);
unsigned twitch_pick_quality(const TwitchQuality *, unsigned count, unsigned preferred_height);
unsigned parse_clip_qualities(const char *body, TwitchQuality out[TWITCH_QUALITIES]);
bool twitch_media_playlist(char *text, TwitchPlaylist *out);
bool twitch_vod_playlist(char *text, TwitchVodSegment **segments, size_t *count, unsigned *duration_ms);

#endif
