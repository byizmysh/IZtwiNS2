/* SPDX-License-Identifier: GPL-3.0-only */
#include "webvtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint64_t pts_period = (UINT64_C(1) << 33) * 1000000 / 90000;

unsigned webvtt_tracks(const char *master, const char *base, WebVttTrack out[WEBVTT_TRACKS]) {
    const char prefix[] = "#EXT-X-MEDIA:";
    unsigned count = 0;
    for (const char *line = master; *line && count < WEBVTT_TRACKS; ) {
        size_t length = strcspn(line, "\r\n");
        if (length > sizeof(prefix) - 1 && length < URL_MAX + 512 &&
            !strncmp(line, prefix, sizeof(prefix) - 1)) {
            char attrs[URL_MAX + 512], type[16], uri[URL_MAX], preferred[8];
            size_t bytes = length - (sizeof(prefix) - 1);
            memcpy(attrs, line + sizeof(prefix) - 1, bytes);
            attrs[bytes] = 0;
            WebVttTrack *track = &out[count];
            if (twitch_attribute(attrs, "TYPE", type, sizeof(type)) && !strcmp(type, "SUBTITLES") &&
                twitch_attribute(attrs, "GROUP-ID", track->group, sizeof(track->group)) &&
                twitch_attribute(attrs, "URI", uri, sizeof(uri)) && twitch_join_url(base, uri, track->url)) {
                if (!twitch_attribute(attrs, "NAME", track->name, sizeof(track->name)))
                    strcpy(track->name, "WebVTT");
                track->preferred = twitch_attribute(attrs, "DEFAULT", preferred, sizeof(preferred)) &&
                    !strcmp(preferred, "YES");
                ++count;
            }
        }
        line += length;
        line += strspn(line, "\r\n");
    }
    return count;
}

const WebVttTrack *webvtt_track(const WebVttTrack *tracks, unsigned count, const char *group) {
    const WebVttTrack *first = NULL;
    if (*group) {
        for (unsigned i = 0; i < count; ++i) {
            if (strcmp(tracks[i].group, group)) continue;
            if (tracks[i].preferred) return &tracks[i];
            if (!first) first = &tracks[i];
        }
    }
    return first;
}

bool webvtt_init_url(char *playlist, const char *base, char url[URL_MAX]) {
    static const char prefix[] = "#EXT-X-MAP:";
    url[0] = 0;
    for (char *line = playlist; *line; ) {
        size_t length = strcspn(line, "\r\n");
        if (length > sizeof(prefix) - 1 && !strncmp(line, prefix, sizeof(prefix) - 1)) {
            char attrs[URL_MAX + 128], uri[URL_MAX], range[80], target[URL_MAX];
            size_t bytes = length - (sizeof(prefix) - 1);
            if (bytes >= sizeof(attrs)) return false;
            memcpy(attrs, line + sizeof(prefix) - 1, bytes);
            attrs[bytes] = 0;
            if (twitch_attribute(attrs, "BYTERANGE", range, sizeof(range)) ||
                !twitch_attribute(attrs, "URI", uri, sizeof(uri)) || !twitch_join_url(base, uri, target) ||
                (*url && strcmp(url, target))) return false;
            strcpy(url, target);
            line[1] = 'V';
        }
        line += length;
        line += strspn(line, "\r\n");
    }
    return true;
}

static bool digits(const char **at, unsigned width, unsigned *value) {
    *value = 0;
    for (unsigned i = 0; i < width; ++i) {
        unsigned digit = (unsigned char)**at - '0';
        if (digit > 9) return false;
        *value = *value * 10 + digit;
        ++*at;
    }
    return true;
}

static bool timestamp(const char **at, uint64_t *us) {
    const char *p = *at;
    size_t width = strspn(p, "0123456789");
    unsigned first, hours = 0, minutes, seconds, millis;
    if (width < 2 || width > 6 || !digits(&p, (unsigned)width, &first) || *p++ != ':' ||
        !digits(&p, 2, &seconds)) return false;
    minutes = first;
    if (*p == ':') {
        ++p;
        hours = first;
        minutes = seconds;
        if (!digits(&p, 2, &seconds)) return false;
    } else if (width != 2) return false;
    if (minutes > 59 || seconds > 59 || *p++ != '.' || !digits(&p, 3, &millis)) return false;
    *us = ((uint64_t)hours * 3600 + minutes * 60 + seconds) * 1000000 + millis * 1000;
    *at = p;
    return true;
}

static bool header(const char *text) {
    return !strncmp(text, "WEBVTT", 6) && (!text[6] || strchr(" \t\r\n", text[6]));
}

static bool timestamp_map(const char *text, uint64_t *local, uint64_t *mpeg) {
    if (!text || !header(text)) return false;
    const char *line = strchr(text, '\n');
    const char key[] = "X-TIMESTAMP-MAP=";
    while (line && *++line && *line != '\r' && *line != '\n') {
        size_t len = strcspn(line, "\r\n");
        if (len >= sizeof(key) && !strncmp(line, key, sizeof(key) - 1)) {
            const char *p = strstr(line, "LOCAL:"), *ticks = strstr(line, "MPEGTS:");
            if (!p || p >= line + len || !ticks || ticks >= line + len) return false;
            p += 6;
            ticks += 7;
            if (!timestamp(&p, local) || *ticks < '0' || *ticks > '9') return false;
            char *end;
            uint64_t raw = strtoull(ticks, &end, 10);
            if (end > line + len || (end < line + len && *end != ',') || raw >= UINT64_C(1) << 33) return false;
            *mpeg = raw * 1000000 / 90000;
            return true;
        }
        line = strchr(line, '\n');
    }
    return false;
}

static char *line_read(char **next) {
    if (!*next || !**next) return NULL;
    char *start = *next, *end = strchr(start, '\n');
    *next = end ? end + 1 : NULL;
    if (!end) end = start + strlen(start);
    if (end > start && end[-1] == '\r') --end;
    *end = 0;
    return start;
}

static void cue_line(char out[CAPTION_BYTES], size_t *used, const char *text) {
    static const struct { const char *escaped; char value; } entities[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&nbsp;", ' '},
    };
    if (*used && *used + 1 < CAPTION_BYTES) out[(*used)++] = '\n';
    while (*text && *used + 1 < CAPTION_BYTES) {
        if (*text == '<') {
            const char *end = strchr(text + 1, '>');
            if (end) { text = end + 1; continue; }
        }
        char value = *text++;
        if (value == '&') {
            for (size_t i = 0; i < sizeof(entities) / sizeof(*entities); ++i) {
                size_t len = strlen(entities[i].escaped);
                if (strncmp(text - 1, entities[i].escaped, len)) continue;
                value = entities[i].value;
                text += len - 1;
                break;
            }
        }
        out[(*used)++] = (unsigned char)value < 32 ? ' ' : value;
    }
    if (*text) {
        while (*used && ((unsigned char)out[*used - 1] & 0xc0) == 0x80) --*used;
        if (*used && (unsigned char)out[*used - 1] >= 0xc0) --*used;
    }
    out[*used] = 0;
}

bool webvtt_parse(char *text, const char *init, WebVttCue *out, unsigned capacity, unsigned *count) {
    *count = 0;
    if (!strncmp(text, "\xef\xbb\xbf", 3)) text += 3;
    if (init && !strncmp(init, "\xef\xbb\xbf", 3)) init += 3;
    bool inline_header = header(text);
    if (!inline_header && (!init || !header(init))) return false;
    uint64_t local = 0, mpeg = 0;
    bool mapped = timestamp_map(inline_header ? text : init, &local, &mpeg);
    char *line = NULL;
    if (inline_header) {
        do { line = line_read(&text); } while (line && *line);
    }
    while ((line = line_read(&text)) && *count < capacity) {
        if (!*line) continue;
        if ((!strncmp(line, "NOTE", 4) && (!line[4] || strchr(" \t", line[4]))) ||
            !strcmp(line, "STYLE") || !strcmp(line, "REGION")) {
            while ((line = line_read(&text)) && *line) {}
            continue;
        }
        if (!strstr(line, "-->")) line = line_read(&text);
        if (!line || !*line) continue;
        const char *at = line;
        uint64_t start, end;
        if (!timestamp(&at, &start) || (*at != ' ' && *at != '\t')) continue;
        at += strspn(at, " \t");
        if (strncmp(at, "-->", 3) || (at[3] != ' ' && at[3] != '\t')) continue;
        at += 3;
        at += strspn(at, " \t");
        if (!timestamp(&at, &end) || (*at && *at != ' ' && *at != '\t') || end <= start) continue;
        WebVttCue *cue = &out[*count];
        cue->start_us = mapped ? (mpeg + start % pts_period + pts_period - local % pts_period) % pts_period : start;
        cue->duration_us = end - start;
        cue->mapped = mapped;
        cue->text[0] = 0;
        size_t used = 0;
        while ((line = line_read(&text)) && *line) cue_line(cue->text, &used, line);
        if (used) ++*count;
    }
    return true;
}

bool webvtt_active(const WebVttCue *cue, uint64_t pts, uint64_t position) {
    if (cue->mapped) return (pts % pts_period + pts_period - cue->start_us) % pts_period < cue->duration_us;
    return position >= cue->start_us && position - cue->start_us < cue->duration_us;
}
