/* SPDX-License-Identifier: GPL-3.0-only
 * PlaybackAccessToken and Usher requests adapted from twiNX app/src/api/twitch.cpp.
 */
#include "twitch.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool ascii_alnum(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool twitch_channel(char *channel) {
    const size_t size = strlen(channel);
    if (!size || size > TWITCH_CHANNEL_MAX) return false;
    for (size_t i = 0; i < size; ++i) {
        char ch = channel[i];
        if (!ascii_alnum(ch) && ch != '_') return false;
        if (ch >= 'A' && ch <= 'Z') channel[i] = ch + ('a' - 'A');
    }
    return true;
}

bool twitch_token_request(const char *channel, char *out, size_t capacity) {
    int n = snprintf(out, capacity,
        "[{\"operationName\":\"PlaybackAccessToken\",\"variables\":{\"isLive\":true,\"isVod\":false,"
        "\"login\":\"%s\",\"vodID\":\"\",\"playerType\":\"embed\",\"platform\":\"site\"},"
        "\"extensions\":{\"persistedQuery\":{\"version\":1,\"sha256Hash\":"
        "\"ed230aa1e33e07eebb8928504583da78a5173989fadfb1ac94be06a04f3cdbe9\"}}}]", channel);
    return n >= 0 && (size_t)n < capacity;
}

bool twitch_video_id(const char *id) {
    size_t size = strlen(id);
    return size && size <= 24 && strspn(id, "0123456789") == size;
}

bool twitch_clip_id(const char *id) {
    size_t size = strlen(id);
    if (!size || size > 95) return false;
    for (size_t i = 0; i < size; ++i)
        if (!ascii_alnum(id[i]) && id[i] != '-' && id[i] != '_') return false;
    return true;
}

bool twitch_vod_request(const char *id, char *out, size_t capacity) {
    if (!twitch_video_id(id)) return false;
    int n = snprintf(out, capacity,
        "{\"operationName\":\"PlaybackAccessToken\",\"variables\":{\"isLive\":false,\"isVod\":true,"
        "\"login\":\"\",\"vodID\":\"%s\",\"playerType\":\"embed\",\"platform\":\"site\"},"
        "\"extensions\":{\"persistedQuery\":{\"version\":1,\"sha256Hash\":"
        "\"ed230aa1e33e07eebb8928504583da78a5173989fadfb1ac94be06a04f3cdbe9\"}}}", id);
    return n >= 0 && (size_t)n < capacity;
}

bool twitch_clip_request(const char *id, char *out, size_t capacity) {
    if (!twitch_clip_id(id)) return false;
    int n = snprintf(out, capacity,
        "{\"operationName\":\"VideoAccessToken_Clip\",\"variables\":{\"slug\":\"%s\",\"platform\":\"web\"},"
        "\"extensions\":{\"persistedQuery\":{\"version\":1,\"sha256Hash\":"
        "\"993d9a5131f15a37bd16f32342c44ed1e0b1a9b968c6afdb662d2cddd595f6c5\"}}}", id);
    return n >= 0 && (size_t)n < capacity;
}

static const char *white(const char *p) {
    while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') ++p;
    return p;
}

static int digit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

static const char *json_string(const char *p, char *out, size_t cap) {
    if (*p++ != '"') return NULL;
    size_t i = 0;
    while (*p && *p != '"') {
        unsigned value = (unsigned char)*p++;
        if (value == '\\') {
            char esc = *p++;
            const char *literal = "\"\\/bfnrt";
            const char *match = esc ? strchr(literal, esc) : NULL;
            if (match) value = (unsigned char)"\"\\/\b\f\n\r\t"[match - literal];
            else if (esc == 'u') {
                value = 0;
                for (unsigned j = 0; j < 4; ++j) {
                    int x = digit(*p++);
                    if (x < 0) return NULL;
                    value = value * 16u + (unsigned)x;
                }
                if (!value || value > 127) return NULL;
            } else return NULL;
        } else if (value < 32) return NULL;
        if (out && i + 1 < cap) out[i] = (char)value;
        ++i;
    }
    if (*p != '"' || (out && i >= cap)) return NULL;
    if (out) out[i] = 0;
    return p + 1;
}

static const char *json_field(const char *p, const char *name) {
    unsigned depth = 0;
    for (; *p; ++p) {
        if (*p == '}' || *p == ']') {
            if (depth <= 1) return NULL;
            --depth;
        } else if (*p == '{' || *p == '[') {
            ++depth;
        } else if (*p == '"') {
            char key[64];
            const char *after = json_string(p, NULL, 0);
            if (!after) return NULL;
            const char *at = white(after);
            if (*at == ':' && json_string(p, key, sizeof(key)) && strcmp(key, name) == 0)
                return white(at + 1);
            p = after - 1;
        }
    }
    return NULL;
}

static bool pct(char **out, const char *end, const char *text) {
    const char *hex = "0123456789ABCDEF";
    for (; *text; ++text) {
        unsigned ch = (unsigned char)*text;
        if (ascii_alnum(*text) || strchr("-._~", *text)) {
            if (end - *out < 2) return false;
            *(*out)++ = *text;
        } else {
            if (end - *out < 4) return false;
            *(*out)++ = '%';
            *(*out)++ = hex[ch >> 4];
            *(*out)++ = hex[ch & 15u];
        }
    }
    **out = 0;
    return true;
}

static bool master_url(const char *key, const char *json, const char *prefix, const char *token_key,
                       char out[URL_MAX]) {
    char token[2048], sig[128];
    const char *object = json_field(json, key);
    if (!object || *object != '{') return false;
    const char *value = json_field(object, "value");
    const char *signature = json_field(object, "signature");
    if (!value || !signature || !json_string(value, token, sizeof(token)) ||
        !json_string(signature, sig, sizeof(sig)) || !*token || !*sig) return false;
    int offset = snprintf(out, URL_MAX, "%s", prefix);
    if (offset < 0 || offset >= URL_MAX) return false;
    char *at = out + offset;
    size_t length = strlen(token_key);
    if (!pct(&at, out + URL_MAX, sig) || (size_t)(out + URL_MAX - at) <= length) return false;
    memcpy(at, token_key, length);
    at += length;
    return pct(&at, out + URL_MAX, token);
}

bool twitch_master_url(const char *channel, const char *json, char out[URL_MAX]) {
    char prefix[384];
    int n = snprintf(prefix, sizeof(prefix), "https://usher.ttvnw.net/api/channel/hls/%s.m3u8?"
        "player=twitchweb&allow_source=true&allow_audio_only=false&type=any&sig=", channel);
    return n > 0 && (size_t)n < sizeof(prefix) && master_url("streamPlaybackAccessToken", json, prefix, "&token=", out);
}

bool twitch_vod_url(const char *video_id, const char *json, char out[URL_MAX]) {
    char prefix[384];
    if (!twitch_video_id(video_id)) return false;
    int n = snprintf(prefix, sizeof(prefix), "https://usher.ttvnw.net/vod/v2/%s.m3u8?"
        "platform=web&player_backend=mediaplayer&supported_codecs=h264&allow_source=true&"
        "allow_audio_only=false&nauthsig=", video_id);
    return n > 0 && (size_t)n < sizeof(prefix) && master_url("videoPlaybackAccessToken", json, prefix, "&nauth=", out);
}

bool twitch_sign_clip(const char *source, const char *token, const char *signature, char out[URL_MAX]) {
    char host[128];
    const char *path;
    if (!twitch_split_https(source, host, &path)) return false;
    int n = snprintf(out, URL_MAX, "%s%csig=", source, strchr(source, '?') ? '&' : '?');
    if (n <= 0 || n >= URL_MAX) return false;
    char *at = out + n;
    if (!pct(&at, out + URL_MAX, signature) || out + URL_MAX - at < 8) return false;
    memcpy(at, "&token=", 7);
    at += 7;
    return pct(&at, out + URL_MAX, token);
}

static bool suffix(const char *host, size_t len, const char *domain) {
    size_t n = strlen(domain);
    return len >= n && !strcmp(host + len - n, domain);
}

bool twitch_split_https(const char *url, char host[128], const char **path) {
    if (strncmp(url, "https://", 8) || strlen(url) >= URL_MAX) return false;
    const char *begin = url + 8, *slash = strchr(begin, '/');
    if (!slash || slash == begin || slash - begin >= 128) return false;
    for (const char *p = begin; p < slash; ++p)
        if (!ascii_alnum(*p) && *p != '-' && *p != '.') return false;
    for (const char *p = slash; *p; ++p)
        if ((unsigned char)*p < 33 || (unsigned char)*p > 126 || *p == '#') return false;
    size_t length = (size_t)(slash - begin);
    memcpy(host, begin, length);
    host[length] = 0;
    *path = slash;
    return !strcmp(host, "gql.twitch.tv") || suffix(host, length, ".ttvnw.net") ||
        suffix(host, length, ".twitchcdn.net") || suffix(host, length, ".cloudfront.net");
}

bool twitch_join_url(const char *base, const char *uri, char out[URL_MAX]) {
    char host[128];
    const char *path = NULL;
    int n;
    if (!strncmp(uri, "https://", 8)) n = snprintf(out, URL_MAX, "%s", uri);
    else {
        if (!*uri || strchr(uri, ':') || !twitch_split_https(base, host, &path)) return false;
        if (uri[0] == '/' && uri[1] == '/') n = snprintf(out, URL_MAX, "https:%s", uri);
        else if (*uri == '/') n = snprintf(out, URL_MAX, "https://%s%s", host, uri);
        else {
            const char *query = strchr(path, '?');
            size_t prefix = query ? (size_t)(query - base) : strlen(base);
            while (prefix && base[prefix - 1] != '/') --prefix;
            n = snprintf(out, URL_MAX, "%.*s%s", (int)prefix, base, uri);
        }
    }
    return n >= 0 && n < URL_MAX && twitch_split_https(out, host, &path);
}

static char *line(char **next) {
    char *start = *next;
    if (!start || !*start) return NULL;
    char *end = strchr(start, '\n');
    *next = end ? end + 1 : NULL;
    if (!end) end = start + strlen(start);
    if (end > start && end[-1] == '\r') --end;
    *end = 0;
    return start;
}

bool twitch_attribute(const char *attributes, const char *key, char *out, size_t capacity) {
    if (out) {
        if (!capacity) return false;
        out[0] = 0;
    }
    while (*attributes) {
        const char *equals = strchr(attributes, '=');
        if (!equals) return false;
        const char *value = equals + 1, *end, *next;
        if (*value == '"') {
            end = strchr(++value, '"');
            if (!end || (end[1] && end[1] != ',')) return false;
            next = end + 1;
        } else {
            end = value + strcspn(value, ",");
            next = end;
        }
        size_t bytes = (size_t)(end - value);
        if ((size_t)(equals - attributes) == strlen(key) && !strncmp(attributes, key, strlen(key))) {
            if (!out) return true;
            if (!bytes || bytes >= capacity) return false;
            memmove(out, value, bytes);
            out[bytes] = 0;
            return true;
        }
        attributes = *next ? next + 1 : next;
    }
    return false;
}

unsigned twitch_qualities(char *text, const char *url, TwitchQuality out[TWITCH_QUALITIES]) {
    char *p = line(&text);
    if (!p || strcmp(p, "#EXTM3U")) return 0;
    unsigned count = 0, height = 0, fps = 0;
    char subtitles[sizeof(out[0].subtitle_group)] = "";
    for (; (p = line(&text)); ) {
        if (!strncmp(p, "#EXT-X-STREAM-INF:", 18)) {
            height = fps = 0;
            twitch_attribute(p + 18, "SUBTITLES", subtitles, sizeof(subtitles));
            const char *dim = strstr(p, "RESOLUTION=");
            if (!dim || (strstr(p, "CODECS=") && !strstr(p, "avc1") && !strstr(p, "avc3"))) continue;
            char *sep, *end;
            long w = strtol(dim + 11, &sep, 10);
            if (*sep != 'x' || w < 1 || w > TWITCH_MAX_WIDTH) continue;
            long h = strtol(sep + 1, &end, 10);
            if ((*end && *end != ',') || h < 144 || h > TWITCH_MAX_HEIGHT) continue;
            height = (unsigned)h;
            const char *rate = strstr(p, "FRAME-RATE=");
            double f = rate ? strtod(rate + 11, NULL) : 0;
            if (f >= 1 && f <= 120) fps = (unsigned)(f + .5);
        } else if (*p && *p != '#' && height) {
            unsigned i = 0;
            while (i < count && out[i].height != height) ++i;
            if (i < TWITCH_QUALITIES && (i == count || (fps && (!out[i].fps || fps < out[i].fps)))) {
                char stream[URL_MAX];
                if (twitch_join_url(url, p, stream)) {
                    strcpy(out[i].url, stream);
                    out[i].height = height;
                    out[i].fps = fps;
                    strcpy(out[i].subtitle_group, subtitles);
                    if (i == count) ++count;
                }
            }
            height = 0;
        }
    }
    return count;
}

unsigned twitch_pick_quality(const TwitchQuality *qualities, unsigned count, unsigned height) {
    if (!height) height = 360;
    unsigned best = 0, distance = UINT_MAX;
    for (unsigned i = 0; i < count; ++i) {
        unsigned h = qualities[i].height;
        unsigned d = h > height ? h - height : height - h;
        if (d < distance || (d == distance && h < qualities[best].height)) {
            best = i;
            distance = d;
        }
    }
    return best;
}

static bool playlist_init(char *tag) {
    char *attributes = tag + sizeof("#EXT-X-MAP:") - 1;
    /* Leave the URI in the playlist buffer so it stays valid for its segments. */
    return !twitch_attribute(attributes, "BYTERANGE", NULL, 0) &&
        twitch_attribute(attributes, "URI", tag, strlen(tag) + 1);
}

bool twitch_media_playlist(char *text, TwitchPlaylist *out) {
    *out = (TwitchPlaylist){0};
    char *p = line(&text);
    if (!p || strcmp(p, "#EXTM3U")) return false;
    bool sequence = false, media = false;
    const char *init = NULL;
    for (; (p = line(&text)); ) {
        if (!strncmp(p, "#EXT-X-MEDIA-SEQUENCE:", 22)) {
            char *end;
            const char *start = p + 22;
            errno = 0;
            out->first = strtoull(start, &end, 10);
            if (sequence || *start < '0' || *start > '9' || *end || errno || out->first > UINT64_MAX - SEGMENTS_MAX) return false;
            sequence = true;
        } else if (!strncmp(p, "#EXTINF:", 8)) media = true;
        else if (!strcmp(p, "#EXT-X-ENDLIST")) out->ended = true;
        else if (!strncmp(p, "#EXT-X-MAP:", 11)) {
            if (!playlist_init(p)) return false;
            init = p;
        } else if (!strncmp(p, "#EXT-X-BYTERANGE:", 17) ||
                 (!strncmp(p, "#EXT-X-KEY:", 11) && strcmp(p + 11, "METHOD=NONE"))) return false;
        else if (*p && *p != '#' && media) {
            if (out->count >= SEGMENTS_MAX) return false;
            out->init[out->count] = init;
            out->uri[out->count++] = p;
            media = false;
        }
    }
    return sequence && out->count;
}

/* Keep segment timestamps for duration display and seeking. */
bool twitch_vod_playlist(char *text, TwitchVodSegment **segments, size_t *count, unsigned *duration_ms) {
    *segments = NULL;
    *count = 0;
    *duration_ms = 0;
    char *p = line(&text);
    if (!p || strcmp(p, "#EXTM3U")) return false;
    bool ended = false;
    unsigned segment_ms = 0, at = 0;
    const char *init = NULL;
    size_t capacity = 0;
    while ((p = line(&text))) {
        if (!strncmp(p, "#EXTINF:", 8)) {
            char *end;
            double seconds = strtod(p + 8, &end);
            if (segment_ms || end == p + 8 || (*end && *end != ',') ||
                !(seconds >= .001 && seconds <= 3600)) break;
            segment_ms = (unsigned)(seconds * 1000 + .5);
        }
        else if (!strcmp(p, "#EXT-X-ENDLIST")) { ended = true; break; }
        else if (!strncmp(p, "#EXT-X-MAP:", 11)) {
            if (!playlist_init(p)) break;
            init = p;
        } else if (!strncmp(p, "#EXT-X-BYTERANGE:", 17) ||
                 (!strncmp(p, "#EXT-X-KEY:", 11) && strcmp(p + 11, "METHOD=NONE"))) break;
        else if (*p && *p != '#' && segment_ms) {
            if (at > UINT_MAX - segment_ms) break;
            if (*count == capacity) {
                if (capacity >= 65536) break;
                capacity = capacity ? capacity * 2 : 256;
                TwitchVodSegment *next = realloc(*segments, capacity * sizeof(**segments));
                if (!next) break;
                *segments = next;
            }
            (*segments)[(*count)++] = (TwitchVodSegment){.uri = p, .at_ms = at, .init = init};
            at += segment_ms;
            segment_ms = 0;
        }
    }
    if (ended && *count && !segment_ms) { *duration_ms = at; return true; }
    free(*segments);
    *segments = NULL;
    *count = 0;
    return false;
}
