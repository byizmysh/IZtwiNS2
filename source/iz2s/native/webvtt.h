/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef IZTWINS2_WEBVTT_H
#define IZTWINS2_WEBVTT_H
#include "media.h"
#include "twitch.h"

enum { WEBVTT_TRACKS = 8, WEBVTT_SEGMENT_CUES = 48, WEBVTT_CACHED_CUES = 96 };

typedef struct {
    char group[64], name[64], url[URL_MAX];
    bool preferred;
} WebVttTrack;

typedef struct {
    uint64_t start_us, duration_us;
    bool mapped;
    char text[CAPTION_BYTES];
} WebVttCue;

unsigned webvtt_tracks(const char *master, const char *base, WebVttTrack out[WEBVTT_TRACKS]);
const WebVttTrack *webvtt_track(const WebVttTrack *, unsigned count, const char *group);
bool webvtt_init_url(char *playlist, const char *base, char url[URL_MAX]);
bool webvtt_parse(char *text, const char *init, WebVttCue *, unsigned capacity, unsigned *count);
bool webvtt_active(const WebVttCue *, uint64_t pts, uint64_t position);

#endif
