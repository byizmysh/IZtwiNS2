/* SPDX-License-Identifier: GPL-3.0-only */
#include "https.h"
#include "account.h"
#include "chat.h"
#include "library.h"
#include "media.h"
#include "menu.h"
#include "typography.h"
#include "preview-cache.h"
#include "twitch.h"
#include "webvtt.h"

#include <switch.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef IZTWINS2_IZA
#include "../iza/player-platform.h"
#include "../iza/native-storage.h"
#else
typedef Framebuffer PlayerDisplay;
#endif

static void *display_begin(PlayerDisplay *display, u32 *stride) {
#ifdef IZTWINS2_IZA
    *stride = UI_WIDTH * sizeof(uint32_t);
    return display->pixels;
#else
    return framebufferBegin(display, stride);
#endif
}

static void display_end(PlayerDisplay *display) {
#ifndef IZTWINS2_IZA
    framebufferEnd(display);
#else
    (void)display;
#endif
}

enum {
    SEGMENT_CAP = 4 * 1024 * 1024, PLAYLIST_CAP = 256 * 1024, INIT_CAP = 512 * 1024, SEG_QUEUE = 4,
    VOD_PLAYLIST_CAP = 8 * 1024 * 1024, CLIP_CAP = 40 * 1024 * 1024,
    AUDIO_SAMPLES = 1024,
    PCM_QUEUE = 24, AUDIO_QUEUE = 6,
};

typedef struct {
    char *data;
    size_t size, capacity;
    unsigned at_ms, skip_ms;
    bool fragmented;
} Segment;

typedef struct {
    char url[URL_MAX];
    char *data;
    size_t size;
} SegmentInit;

typedef struct {
    uint8_t *rgba;
    size_t capacity, bytes;
    unsigned width, height;
    MediaPixelFormat format;
} PlaybackFrame;

typedef struct {
    atomic_bool stop, paused, network_done, decode_done, reconnecting;
    atomic_bool subtitle_stop, subtitle_done, want_captions;
    atomic_uint segment_in, segment_out, pcm_in, pcm_out, video_in, video_out;
    atomic_uint quality_count, duration_ms, position_ms;
    atomic_uint_fast64_t video_pts;
    unsigned preferred_quality, resume_ms, playing_quality, playing_fps, recommended_quality;
    unsigned qualities[TWITCH_QUALITIES], quality_fps[TWITCH_QUALITIES];
    char channel[TWITCH_CHANNEL_MAX + 1], id[96], network_error[128], decode_error[128];
    ItemKind kind;
    Segment segments[SEG_QUEUE];
    SegmentInit init;
    int16_t pcm[PCM_QUEUE][AUDIO_SAMPLES * 2];
    unsigned pcm_used;
    PlaybackFrame video[2], shown;
    bool has_frame;
    int64_t first_pts, last_pts, segment_pts;
    unsigned segment_at_ms;
    uint64_t first_ns;
    Mutex caption_lock;
    char caption[CAPTION_BYTES];
    bool caption_seen;
    WebVttTrack subtitle;
    WebVttCue *subtitle_cues;
    unsigned subtitle_count, subtitle_next;
    bool subtitle_ready;
    Thread network, decoder, subtitle_thread;
    bool network_started, decoder_started, subtitle_started;
#ifdef IZTWINS2_IZA
    uint64_t downloaded_max, segment_wait, last_tick, max_tick, max_draw;
    unsigned waits, dropped, late, uploads, yuv_uploads, stalls, audio_empty, ticks33, chat_ticks;
    bool waiting_video, audio_underflow;
#endif
} Playback;

typedef struct {
    bool open, started;
    bool pending[AUDIO_QUEUE];
    AudioOutBuffer buffers[AUDIO_QUEUE];
    unsigned submitted;
} Audio;

static _Alignas(4096) int16_t samples[AUDIO_QUEUE][AUDIO_SAMPLES * 2];
static uint32_t chat_pixels[UI_CHAT_WIDTH * UI_HEIGHT];

static uint64_t now_ns(void) { return armTicksToNs(armGetSystemTick()); }
static bool cancelled(void *opaque) { return atomic_load(&((Playback *)opaque)->stop); }

static void publish_quality(Playback *p, const TwitchQuality *qualities, unsigned count, unsigned selected) {
    for (unsigned i = 0; i < count; ++i) {
        unsigned j = i;
        while (j && p->qualities[j - 1] < qualities[i].height) {
            p->qualities[j] = p->qualities[j - 1];
            p->quality_fps[j] = p->quality_fps[j - 1];
            --j;
        }
        p->qualities[j] = qualities[i].height;
        p->quality_fps[j] = qualities[i].fps;
    }
    p->playing_quality = qualities[selected].height;
    p->playing_fps = qualities[selected].fps;
    p->recommended_quality = qualities[twitch_pick_quality(qualities, count, 0)].height;
    atomic_store(&p->quality_count, count);
}

static bool choose_quality(Playback *p, const TwitchQuality *qualities, unsigned count, char url[URL_MAX]) {
    if (!count) return false;
    const unsigned selected = twitch_pick_quality(qualities, count, p->preferred_quality);
    publish_quality(p, qualities, count, selected);
    strcpy(url, qualities[selected].url);
    return true;
}

static bool download_segment(Https *net, Segment *segment, const char *url, size_t capacity, const SegmentInit *init) {
    size_t prefix = init ? init->size : 0;
    if (segment->capacity < capacity + prefix) {
        char *buffer = realloc(segment->data, capacity + prefix);
        if (!buffer) {
            snprintf(net->error, sizeof(net->error), "Not enough memory to buffer this quality.");
            return false;
        }
        segment->data = buffer;
        segment->capacity = capacity + prefix;
    }
    size_t size;
    if (!https_fetch(net, url, NULL, segment->data + prefix, capacity, &size, NULL)) return false;
    if (!size) {
        snprintf(net->error, sizeof(net->error), "Twitch returned empty video data.");
        return false;
    }
    if (prefix) memcpy(segment->data, init->data, prefix);
    segment->size = prefix + size;
    segment->fragmented = init != NULL;
    return true;
}

static bool download_hls_segment(Playback *p, Https *net, Segment *segment, const char *base,
                                 const char *uri, const char *map, size_t capacity) {
#ifdef IZTWINS2_IZA
    uint64_t started = now_ns();
#endif
    char url[URL_MAX], init_url[URL_MAX];
    if (!twitch_join_url(base, uri, url) || (map && !twitch_join_url(base, map, init_url))) {
        snprintf(net->error, sizeof(net->error), "Twitch returned an invalid segment URL.");
        return false;
    }
    SegmentInit *init = map ? &p->init : NULL;
    if (init && strcmp(init->url, init_url)) {
        if (!init->data) init->data = malloc(INIT_CAP);
        if (!init->data) {
            snprintf(net->error, sizeof(net->error), "Not enough memory for the video initialization segment.");
            return false;
        }
        init->url[0] = 0;
        if (!https_fetch(net, init_url, NULL, init->data, INIT_CAP, &init->size, NULL)) return false;
        if (!init->size) {
            snprintf(net->error, sizeof(net->error), "Twitch returned an empty video initialization segment.");
            return false;
        }
        strcpy(init->url, init_url);
    }
    bool loaded = download_segment(net, segment, url, capacity, init);
#ifdef IZTWINS2_IZA
    uint64_t elapsed = now_ns() - started;
    if (elapsed > p->downloaded_max) p->downloaded_max = elapsed;
#endif
    return loaded;
}

static bool subtitle_segment(Playback *p, Https *net, const char *base, const char *uri,
                             const char *init, char *response, WebVttCue *cues) {
    char url[URL_MAX];
    size_t bytes;
    unsigned count;
    if (!twitch_join_url(base, uri, url) || !https_fetch(net, url, NULL, response, PLAYLIST_CAP, &bytes, NULL) ||
        !webvtt_parse(response, init, cues, WEBVTT_SEGMENT_CUES, &count)) return false;
    mutexLock(&p->caption_lock);
    for (unsigned i = 0; i < count; ++i) {
        p->subtitle_cues[p->subtitle_next] = cues[i];
        p->subtitle_next = (p->subtitle_next + 1) % WEBVTT_CACHED_CUES;
        if (p->subtitle_count < WEBVTT_CACHED_CUES) ++p->subtitle_count;
    }
    p->subtitle_ready = true;
    mutexUnlock(&p->caption_lock);
    return true;
}

static bool subtitle_playlist(Playback *p, Https *net, char *playlist, size_t capacity, char base[URL_MAX],
                              char *init, char previous_init[URL_MAX]) {
    char init_url[URL_MAX];
    size_t bytes;
    if (!https_fetch(net, p->subtitle.url, NULL, playlist, capacity, &bytes, base) ||
        !webvtt_init_url(playlist, base, init_url)) return false;
    if (!strcmp(init_url, previous_init)) return true;
    if (*init_url && !https_fetch(net, init_url, NULL, init, PLAYLIST_CAP, &bytes, NULL)) return false;
    if (!*init_url) *init = 0;
    strcpy(previous_init, init_url);
    return true;
}

static void live_subtitles(Playback *p, Https *net, char *playlist, char *init, char *response, WebVttCue *cues) {
    char base[URL_MAX], previous_init[URL_MAX] = "";
    uint64_t next = 0;
    unsigned failures = 0;
    while (!atomic_load(&p->subtitle_stop) && failures < 5) {
        if (!atomic_load(&p->want_captions)) { svcSleepThread(200000000); continue; }
        TwitchPlaylist parsed;
        if (!subtitle_playlist(p, net, playlist, PLAYLIST_CAP, base, init, previous_init) ||
            !twitch_media_playlist(playlist, &parsed)) ++failures;
        else {
            uint64_t end = parsed.first + parsed.count;
            if (next < parsed.first || !next) next = parsed.count > 4 ? end - 4 : parsed.first;
            while (next < end && !atomic_load(&p->subtitle_stop)) {
                if (!subtitle_segment(p, net, base, parsed.uri[next - parsed.first], init, response, cues)) {
                    if (++failures >= 3) ++next;
                    break;
                }
                failures = 0;
                ++next;
            }
            if (parsed.ended && next >= end) break;
        }
        for (unsigned i = 0; i < 10 && !atomic_load(&p->subtitle_stop); ++i) svcSleepThread(100000000);
    }
}

static void recording_subtitles(Playback *p, Https *net, char *playlist, char *init, char *response, WebVttCue *cues) {
    char base[URL_MAX], previous_init[URL_MAX] = "";
    TwitchVodSegment *index;
    size_t count;
    unsigned duration;
    if (!subtitle_playlist(p, net, playlist, VOD_PLAYLIST_CAP, base, init, previous_init) ||
        !twitch_vod_playlist(playlist, &index, &count, &duration)) return;
    size_t next = 0;
    unsigned target = atomic_load(&p->position_ms), tries = 0;
    while (next + 1 < count && index[next + 1].at_ms <= target) ++next;
    while (next < count && !atomic_load(&p->subtitle_stop)) {
        uint64_t ahead = (uint64_t)atomic_load(&p->position_ms) + 10000;
        if (atomic_load(&p->want_captions) && index[next].at_ms <= ahead) {
            unsigned position = atomic_load(&p->position_ms);
            while (next + 1 < count && index[next + 1].at_ms < position) ++next;
            if (subtitle_segment(p, net, base, index[next].uri, init, response, cues) || ++tries >= 3) {
                ++next;
                tries = 0;
                continue;
            }
        }
        svcSleepThread(200000000);
    }
    free(index);
}

static void subtitle_worker(void *opaque) {
    Playback *p = opaque;
    while (!atomic_load(&p->want_captions) && !atomic_load(&p->subtitle_stop)) svcSleepThread(100000000);
    if (atomic_load(&p->subtitle_stop)) {
        atomic_store(&p->subtitle_done, true);
        return;
    }
    char *playlist = malloc(p->kind == ITEM_VIDEO ? VOD_PLAYLIST_CAP : PLAYLIST_CAP);
    char *init = calloc(1, PLAYLIST_CAP), *response = malloc(PLAYLIST_CAP);
    WebVttCue *cues = malloc(WEBVTT_SEGMENT_CUES * sizeof(*cues));
    Https net = {0};
    if (playlist && init && response && cues && https_begin(&net, &p->subtitle_stop)) {
        if (p->kind == ITEM_LIVE) live_subtitles(p, &net, playlist, init, response, cues);
        else recording_subtitles(p, &net, playlist, init, response, cues);
    }
    https_end(&net);
    free(cues);
    free(response);
    free(init);
    free(playlist);
    atomic_store(&p->subtitle_done, true);
}

static void start_subtitles(Playback *p, const WebVttTrack *track) {
    if (!track || cancelled(p)) return;
    p->subtitle_cues = malloc(WEBVTT_CACHED_CUES * sizeof(*p->subtitle_cues));
    if (!p->subtitle_cues) return;
    p->subtitle = *track;
    atomic_store(&p->subtitle_done, false);
    if (R_SUCCEEDED(threadCreate(&p->subtitle_thread, subtitle_worker, p, NULL, 0x30000, 0x32, -2))) {
        if (R_SUCCEEDED(threadStart(&p->subtitle_thread))) {
            p->subtitle_started = true;
            return;
        }
        threadClose(&p->subtitle_thread);
    }
    atomic_store(&p->subtitle_done, true);
}

static bool fetch_live(Playback *p, Https *net, char *response) {
    char request[512], master[URL_MAX], media[URL_MAX], base[URL_MAX];
    TwitchQuality qualities[TWITCH_QUALITIES];
    size_t size;
    if (!twitch_token_request(p->channel, request, sizeof(request)) ||
        !https_fetch(net, "https://gql.twitch.tv/gql", request, response, PLAYLIST_CAP, &size, NULL)) return false;
    if (!twitch_master_url(p->channel, response, master)) {
        snprintf(net->error, sizeof(net->error), "Channel unavailable or Twitch refused playback");
        return false;
    }
    if (!https_fetch(net, master, NULL, response, PLAYLIST_CAP, &size, base)) return false;
    WebVttTrack subtitles[WEBVTT_TRACKS];
    unsigned subtitle_count = webvtt_tracks(response, base, subtitles);
    unsigned count = twitch_qualities(response, base, qualities);
    if (!count) {
        snprintf(net->error, sizeof(net->error), "No compatible H.264 stream up to 720p");
        return false;
    }

    unsigned selected = twitch_pick_quality(qualities, count, p->preferred_quality);
    uint64_t next = 0, last_segment = now_ns();
    bool ready = false;
    TwitchPlaylist playlist;
    for (unsigned attempt = 0; attempt < count && !cancelled(p); ++attempt) {
        const unsigned rendition = (selected + attempt) % count;
        strcpy(media, qualities[rendition].url);
        if (!https_fetch(net, media, NULL, response, PLAYLIST_CAP, &size, base)) continue;
        if (!twitch_media_playlist(response, &playlist)) {
            snprintf(net->error, sizeof(net->error), "Twitch returned an unsupported live playlist.");
            continue;
        }
        unsigned begin = playlist.count > 3 ? playlist.count - 3 : 0;
        for (unsigned i = begin; i < playlist.count && !cancelled(p); ++i) {
            if (!download_hls_segment(p, net, &p->segments[0], base, playlist.uri[i], playlist.init[i],
                                      qualities[rendition].height > 480 ? SEGMENT_CAP * 2 : SEGMENT_CAP))
                continue;
            publish_quality(p, qualities, count, rendition);
            start_subtitles(p, webvtt_track(subtitles, subtitle_count, qualities[rendition].subtitle_group));
            atomic_store(&p->segment_in, 1);
            next = playlist.first + i + 1;
            last_segment = now_ns();
            ready = true;
            break;
        }
        if (ready) break;
    }
    if (!ready) return false;
    const size_t segment_capacity = p->playing_quality > 480 ? SEGMENT_CAP * 2 : SEGMENT_CAP;
    unsigned failures = 0;
    bool initial_playlist = true;
    while (!cancelled(p)) {
        const bool using_initial = initial_playlist;
        initial_playlist = false;
        bool received = using_initial || https_fetch(net, media, NULL, response, PLAYLIST_CAP, &size, base);
        if (!received || (!using_initial && !twitch_media_playlist(response, &playlist))) {
            if (received) snprintf(net->error, sizeof(net->error), "Live playlist is empty or uses unsupported segments.");
            atomic_store(&p->reconnecting, true);
            if (++failures >= 3) return false;
            svcSleepThread(500000000);
            continue;
        }
        const uint64_t end = playlist.first + playlist.count;
        if (next < playlist.first) next = playlist.first;
        bool retry = false;
        while (next < end && !cancelled(p)) {
            unsigned write = atomic_load(&p->segment_in);
            if (write - atomic_load(&p->segment_out) >= SEG_QUEUE) { svcSleepThread(4000000); continue; }
            Segment *segment = &p->segments[write % SEG_QUEUE];
            unsigned index = (unsigned)(next - playlist.first);
            if (!download_hls_segment(p, net, segment, base, playlist.uri[index], playlist.init[index], segment_capacity)) {
                atomic_store(&p->reconnecting, true);
                if (++failures >= 3) return false;
                retry = true;
                break;
            }
            failures = 0;
            atomic_store(&p->reconnecting, false);
            atomic_store(&p->segment_in, write + 1);
            ++next;
            last_segment = now_ns();
        }
        if (playlist.ended && !retry) return true;
        if (atomic_load(&p->segment_in) == atomic_load(&p->segment_out) &&
            now_ns() - last_segment >= UINT64_C(12000000000)) {
            snprintf(net->error, sizeof(net->error), "Twitch stopped sending stream segments");
            return false;
        }
        if (!cancelled(p)) svcSleepThread(550000000);
    }
    return true;
}

static bool fetch_recording(Playback *p, Https *net, char *response, size_t capacity) {
    char request[512], master[URL_MAX], media[URL_MAX], base[URL_MAX];
    TwitchQuality qualities[TWITCH_QUALITIES];
    const bool clip = p->kind == ITEM_CLIP;
    bool requested = clip ? twitch_clip_request(p->id, request, sizeof(request)) :
                            twitch_vod_request(p->id, request, sizeof(request));
    size_t size;
    if (!requested || !https_fetch(net, "https://gql.twitch.tv/gql", request, response, capacity, &size, NULL))
        return false;
    if (clip) {
        Segment *segment = &p->segments[0];
        if (!choose_quality(p, qualities, parse_clip_qualities(response, qualities), media)) {
            snprintf(net->error, sizeof(net->error), "This public clip is unavailable.");
            return false;
        }
        if (!download_segment(net, segment, media, p->playing_quality > 480 ? CLIP_CAP : 24 * 1024 * 1024, NULL))
            return false;
        atomic_store(&p->segment_in, 1);
        return true;
    }
    if (!twitch_vod_url(p->id, response, master)) {
        snprintf(net->error, sizeof(net->error), "This public video is unavailable.");
        return false;
    }
    if (!https_fetch(net, master, NULL, response, capacity, &size, base)) return false;
    WebVttTrack subtitles[WEBVTT_TRACKS];
    unsigned subtitle_count = webvtt_tracks(response, base, subtitles);
    unsigned quality_count = twitch_qualities(response, base, qualities);
    if (!choose_quality(p, qualities, quality_count, media)) {
        snprintf(net->error, sizeof(net->error), "This video has no compatible H.264 quality.");
        return false;
    }
    TwitchVodSegment *index;
    size_t count;
    unsigned duration;
    if (!https_fetch(net, media, NULL, response, capacity, &size, base)) return false;
    if (!twitch_vod_playlist(response, &index, &count, &duration)) {
        snprintf(net->error, sizeof(net->error), "This video playlist is unsupported.");
        return false;
    }
    size_t first = 0;
    unsigned last = duration > 1000 ? duration - 1000 : 0;
    unsigned target = p->resume_ms < last ? p->resume_ms : last;
    while (first + 1 < count && index[first + 1].at_ms <= target) ++first;
    atomic_store(&p->position_ms, target);
    atomic_store(&p->duration_ms, duration);
    unsigned selected = twitch_pick_quality(qualities, quality_count, p->preferred_quality);
    start_subtitles(p, webvtt_track(subtitles, subtitle_count, qualities[selected].subtitle_group));
    bool ok = true;
    size_t segment_capacity = p->playing_quality > 480 ? SEGMENT_CAP * 2 : SEGMENT_CAP;
    for (size_t i = first; i < count && !cancelled(p); ++i) {
        unsigned write = atomic_load(&p->segment_in);
        while (!cancelled(p) && write - atomic_load(&p->segment_out) >= SEG_QUEUE) svcSleepThread(4000000);
        if (cancelled(p)) break;
        Segment *segment = &p->segments[write % SEG_QUEUE];
        segment->at_ms = index[i].at_ms;
        segment->skip_ms = i == first ? target - segment->at_ms : 0;
        unsigned retries = 0;
        while (!cancelled(p) &&
            !download_hls_segment(p, net, segment, base, index[i].uri, index[i].init, segment_capacity)) {
            atomic_store(&p->reconnecting, true);
            if (++retries == 3) break;
            svcSleepThread(250000000);
        }
        if (retries == 3) { ok = false; break; }
        atomic_store(&p->reconnecting, false);
        if (!cancelled(p)) atomic_store(&p->segment_in, write + 1);
    }
    free(index);
    return ok;
}

static void network_thread(void *opaque) {
    Playback *p = opaque;
    size_t capacity = p->kind == ITEM_VIDEO ? VOD_PLAYLIST_CAP : PLAYLIST_CAP;
    char *response = malloc(capacity);
    Https net = {0};
    if (!response) snprintf(p->network_error, sizeof(p->network_error), "Not enough memory for playlists");
    else {
        bool ok = https_begin(&net, &p->stop) && (p->kind == ITEM_LIVE ? fetch_live(p, &net, response) :
                  fetch_recording(p, &net, response, capacity));
        if (!ok && !cancelled(p))
            snprintf(p->network_error, sizeof(p->network_error), "%s", net.error[0] ? net.error : "Twitch request failed");
        https_end(&net);
    }
    free(response);
    atomic_store(&p->network_done, true);
}

static uint64_t wait_paused(Playback *p) {
    uint64_t start = now_ns();
    while (atomic_load(&p->paused) && !cancelled(p)) svcSleepThread(4000000);
    uint64_t elapsed = now_ns() - start;
    if (p->first_ns) p->first_ns += elapsed;
    return elapsed;
}

static bool frame_buffer(PlaybackFrame *frame, unsigned width, unsigned height,
                         size_t bytes, MediaPixelFormat format) {
    if (frame->capacity < bytes) {
        uint8_t *buffer = realloc(frame->rgba, bytes);
        if (!buffer) return false;
        frame->rgba = buffer;
        frame->capacity = bytes;
    }
    frame->width = width;
    frame->height = height;
    frame->bytes = bytes;
    frame->format = format;
    return true;
}

static bool next_video(Playback *p, int64_t pts, unsigned width, unsigned height,
                       size_t bytes, MediaPixelFormat format, PlaybackFrame **result) {
    *result = NULL;
    if (atomic_load(&p->paused)) wait_paused(p);
    uint64_t now = now_ns();
    if (pts < 0) pts = p->last_pts ? p->last_pts + 33333 : 0;
    if (!p->first_ns || pts < p->first_pts || pts - p->last_pts > 4500000) {
        p->first_pts = pts;
        p->first_ns = now + 100000000;
    }
    p->last_pts = pts;
    uint64_t show = p->first_ns + (uint64_t)(pts - p->first_pts) * 1000;
    if (now > show + 500000000) { p->first_ns = now; p->first_pts = pts; show = now; }
    while (now < show && !cancelled(p)) {
        if (atomic_load(&p->paused)) {
            show += wait_paused(p);
            now = now_ns();
            continue;
        }
        uint64_t wait = show - now;
        svcSleepThread(wait < 5000000 ? wait : 5000000);
        now = now_ns();
    }
    if (cancelled(p)) return false;
    atomic_store(&p->video_pts, (uint64_t)pts);
    if (p->segment_pts < 0) p->segment_pts = pts;
    if (p->kind != ITEM_LIVE && pts >= p->segment_pts) {
        uint64_t at = p->segment_at_ms + (uint64_t)(pts - p->segment_pts) / 1000;
        unsigned duration = atomic_load(&p->duration_ms);
        atomic_store(&p->position_ms, at <= duration ? (unsigned)at : duration);
    }
    unsigned write = atomic_load(&p->video_in);
    if (write - atomic_load(&p->video_out) >= 2 || now > show + 100000000) {
#ifdef IZTWINS2_IZA
        if (now > show + 100000000) ++p->late;
        else ++p->dropped;
#endif
        return true;
    }
    PlaybackFrame *frame = &p->video[write % 2];
    if (!frame_buffer(frame, width, height, bytes, format)) return false;
    *result = frame;
    return true;
}

static void publish_video(Playback *p) {
    atomic_fetch_add(&p->video_in, 1);
}

static bool video_frame(void *opaque, const uint8_t *rgba, int64_t pts, unsigned width, unsigned height) {
    Playback *p = opaque;
    PlaybackFrame *frame;
    size_t bytes = (size_t)width * height * 4;
    if (!next_video(p, pts, width, height, bytes, MEDIA_RGBA, &frame)) return false;
    if (frame) {
        memcpy(frame->rgba, rgba, bytes);
        publish_video(p);
    }
    return true;
}

#ifdef IZTWINS2_IZA
static bool video_yuv(void *opaque, const MediaYuvFrame *source, int64_t pts) {
    Playback *p = opaque;
    PlaybackFrame *frame;
    size_t luma = (size_t)source->width * source->height;
    unsigned chroma_width = (source->width + 1) / 2, chroma_height = (source->height + 1) / 2;
    size_t bytes = luma + 2 * (size_t)chroma_width * chroma_height;
    size_t aligned = (bytes + 3) & ~(size_t)3;
    if (!next_video(p, pts, source->width, source->height, aligned, source->format, &frame)) return false;
    if (!frame) return true;
    uint8_t *destination = frame->rgba;
    for (unsigned part = 0; part < 3; ++part) {
        unsigned width = part ? chroma_width : source->width;
        unsigned height = part ? chroma_height : source->height;
        const uint8_t *row = source->plane[part];
        for (unsigned y = 0; y < height; ++y) {
            memcpy(destination, row, width);
            destination += width;
            row += source->stride[part];
        }
    }
    memset(destination, 0, aligned - bytes);
    publish_video(p);
    return true;
}
#endif

static bool pcm_frames(void *opaque, const int16_t *pcm, unsigned count) {
    Playback *p = opaque;
    while (count && !cancelled(p)) {
        if (atomic_load(&p->paused)) { wait_paused(p); continue; }
        unsigned write = atomic_load(&p->pcm_in);
        if (write - atomic_load(&p->pcm_out) >= PCM_QUEUE) { svcSleepThread(2000000); continue; }
        unsigned n = AUDIO_SAMPLES - p->pcm_used;
        if (n > count) n = count;
        memcpy(p->pcm[write % PCM_QUEUE] + p->pcm_used * 2, pcm, n * 4);
        p->pcm_used += n;
        pcm += n * 2;
        count -= n;
        if (p->pcm_used == AUDIO_SAMPLES) {
            p->pcm_used = 0;
            atomic_store(&p->pcm_in, write + 1);
        }
    }
    return !cancelled(p);
}

static void caption_frame(void *opaque, const char *caption) {
    Playback *p = opaque;
    mutexLock(&p->caption_lock);
    snprintf(p->caption, sizeof(p->caption), "%s", caption);
    p->caption_seen |= *caption != 0;
    mutexUnlock(&p->caption_lock);
}

static void clip_duration(void *opaque, unsigned ms) {
    Playback *p = opaque;
    atomic_store(&p->duration_ms, ms);
}

static void decoder_thread(void *opaque) {
    Playback *p = opaque;
    MediaOutput out = {
        .user = p, .cancelled = cancelled, .video = video_frame, .audio = pcm_frames,
        .captions = media_captions_open(), .caption = caption_frame, .clip_duration = clip_duration,
#ifdef IZTWINS2_IZA
        .video_yuv = video_yuv,
#endif
    };
    unsigned failures = 0;
#ifdef IZTWINS2_IZA
    uint64_t waiting_at = 0;
#endif
    while (!cancelled(p)) {
        unsigned read = atomic_load(&p->segment_out);
        const bool ended = atomic_load(&p->network_done);
        if (read == atomic_load(&p->segment_in)) {
            if (ended) break;
#ifdef IZTWINS2_IZA
            if (read && !waiting_at && !atomic_load(&p->paused)) {
                waiting_at = now_ns();
                ++p->waits;
            }
#endif
            svcSleepThread(4000000);
            continue;
        }
#ifdef IZTWINS2_IZA
        if (waiting_at) {
            p->segment_wait += now_ns() - waiting_at;
            waiting_at = 0;
        }
#endif
        const Segment *segment = &p->segments[read % SEG_QUEUE];
        p->segment_at_ms = segment->at_ms + segment->skip_ms;
        p->segment_pts = -1;
        out.skip_ms = segment->skip_ms;
        const uint8_t *data = (const uint8_t *)segment->data;
        int rc = p->kind == ITEM_CLIP ? decode_clip(data, segment->size, &out) :
            segment->fragmented ? decode_fragment(data, segment->size, &out) : decode_segment(data, segment->size, &out);
        atomic_store(&p->segment_out, read + 1);
        if (rc >= 0) failures = 0;
        else if (!cancelled(p) && (++failures >= 3 || p->kind == ITEM_CLIP)) {
            snprintf(p->decode_error, sizeof(p->decode_error), "Could not decode video (%d).", rc);
            atomic_store(&p->stop, true);
            break;
        }
    }
#ifdef IZTWINS2_IZA
    if (waiting_at) p->segment_wait += now_ns() - waiting_at;
#endif
    if (!cancelled(p) && p->pcm_used) {
        static const int16_t tail[AUDIO_SAMPLES * 2];
        pcm_frames(p, tail, AUDIO_SAMPLES - p->pcm_used);
    }
    media_captions_close(&out.captions);
    atomic_store(&p->decode_done, true);
}

static bool audio_open(Audio *audio) {
    if (R_FAILED(audoutInitialize())) return false;
    if (audoutGetSampleRate() != AUDIO_RATE || audoutGetChannelCount() != 2) { audoutExit(); return false; }
    audio->open = true;
    return true;
}

static void audio_close(Audio *audio) {
    if (!audio->open) return;
    bool flushed;
    if (audio->started) audoutStopAudioOut();
    audoutFlushAudioOutBuffers(&flushed);
    audoutExit();
    audio->open = false;
}

static bool audio_pump(Audio *audio, Playback *p) {
    AudioOutBuffer *released = NULL;
    u32 count = 0;
    for (unsigned attempt = 0; attempt < AUDIO_QUEUE; ++attempt) {
        Result rc = audoutGetReleasedAudioOutBuffer(&released, &count);
        if (R_FAILED(rc)) return false;
        if (!count) break;
        for (unsigned i = 0; i < AUDIO_QUEUE; ++i)
            if (&audio->buffers[i] == released) audio->pending[i] = false;
    }
    for (unsigned i = 0; i < AUDIO_QUEUE; ++i) {
        unsigned read = atomic_load(&p->pcm_out);
        if (read == atomic_load(&p->pcm_in)) break;
        if (audio->pending[i]) continue;
        memcpy(samples[i], p->pcm[read % PCM_QUEUE], sizeof(samples[i]));
        audio->buffers[i] = (AudioOutBuffer){.buffer = samples[i], .buffer_size = sizeof(samples[i]), .data_size = sizeof(samples[i])};
        if (R_FAILED(audoutAppendAudioOutBuffer(&audio->buffers[i]))) return false;
        audio->pending[i] = true;
        ++audio->submitted;
        atomic_store(&p->pcm_out, read + 1);
    }
#ifdef IZTWINS2_IZA
    bool queued = false;
    for (unsigned i = 0; i < AUDIO_QUEUE; ++i) queued |= audio->pending[i];
    const bool empty = audio->started && !queued && !atomic_load(&p->decode_done);
    if (empty && !p->audio_underflow) ++p->audio_empty;
    p->audio_underflow = empty;
#endif
    if (!audio->started && audio->submitted &&
        (audio->submitted >= 4 || atomic_load(&p->decode_done))) {
        if (R_FAILED(audoutStartAudioOut())) return false;
        audio->started = true;
    }
    return true;
}

#ifdef IZTWINS2_IZA
static void save_playback_performance(const Playback *p) {
    if (!p->uploads) return;
    char text[432];
    int size = snprintf(text, sizeof(text),
        "video_frames=%u yuv_frames=%u chat_callbacks=%u dropped_queue=%u dropped_late=%u\n"
        "callback_over_33ms=%u max_callback_gap_ms=%llu max_draw_ms=%llu\n"
        "queued_segment_waits=%u total_segment_wait_ms=%llu max_download_ms=%llu\n"
        "audio_empty_events=%u buffering_events=%u\n",
        p->uploads, p->yuv_uploads, p->chat_ticks, p->dropped, p->late,
        p->ticks33, (unsigned long long)(p->max_tick / 1000000), (unsigned long long)(p->max_draw / 1000000),
        p->waits, (unsigned long long)(p->segment_wait / 1000000), (unsigned long long)(p->downloaded_max / 1000000),
        p->audio_empty, p->stalls);
    if (size > 0 && (size_t)size < sizeof(text)) iztwi_storage_note("playback-performance.txt", text, (size_t)size);
}
#endif

static void clear(Playback *p) {
    if (!p) return;
    atomic_store(&p->stop, true);
    atomic_store(&p->subtitle_stop, true);
    if (p->network_started) threadWaitForExit(&p->network);
    if (p->decoder_started) threadWaitForExit(&p->decoder);
    if (p->subtitle_started) {
        threadWaitForExit(&p->subtitle_thread);
        threadClose(&p->subtitle_thread);
    }
    if (p->network.handle) threadClose(&p->network);
    if (p->decoder.handle) threadClose(&p->decoder);
#ifdef IZTWINS2_IZA
    save_playback_performance(p);
#endif
    for (unsigned i = 0; i < SEG_QUEUE; ++i) free(p->segments[i].data);
    free(p->init.data);
    for (unsigned i = 0; i < 2; ++i) free(p->video[i].rgba);
    free(p->shown.rgba);
    free(p->subtitle_cues);
    free(p);
}

static Playback *start(const Channel *item, unsigned quality, unsigned resume_ms, bool captions_on) {
    Playback *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->kind = item->kind;
    p->preferred_quality = quality;
    p->resume_ms = resume_ms;
    atomic_store(&p->position_ms, resume_ms);
    atomic_store(&p->subtitle_done, true);
    atomic_store(&p->want_captions, captions_on);
    mutexInit(&p->caption_lock);
    strcpy(p->channel, item->login);
    strcpy(p->id, item->id);
    if (R_FAILED(threadCreate(&p->network, network_thread, p, NULL, 0x30000, 0x2c, -2)) ||
        R_FAILED(threadCreate(&p->decoder, decoder_thread, p, NULL, 0x60000, 0x2c, -2)) ||
        R_FAILED(threadStart(&p->network))) { clear(p); return NULL; }
    p->network_started = true;
    if (R_FAILED(threadStart(&p->decoder))) { clear(p); return NULL; }
    p->decoder_started = true;
    return p;
}

static PlaybackFrame *take_latest(Playback *p) {
    unsigned read = atomic_load(&p->video_out);
    unsigned write = atomic_load(&p->video_in);
    if (read == write) return NULL;
    /* The decoder cannot reuse this slot until video_out advances. Its
       former allocation remains available to redraw and restart playback. */
    PlaybackFrame *slot = &p->video[(write - 1) % 2];
    PlaybackFrame previous = p->shown;
    p->shown = *slot;
    *slot = previous;
    p->has_frame = true;
    atomic_store(&p->video_out, write);
    return &p->shown;
}

#ifdef IZTWINS2_IZA
static bool draw_video(PlayerDisplay *display, Playback *p, const PlayerOverlay *overlay,
                       bool redraw, bool chat_dirty) {
    VideoSurface *surface = display->surface;
    bool layout_changed = !surface->playback || surface->chat != overlay->chat;
    const PlaybackFrame *latest = take_latest(p);
    if (!latest && layout_changed && p->has_frame) latest = &p->shown;
    if (latest) {
        memcpy(display->video, latest->rgba, latest->bytes);
        ++p->uploads;
        p->yuv_uploads += latest->format != MEDIA_RGBA;
    }
    surface->playback = true;
    surface->chat = overlay->chat;
    surface->width = p->has_frame ? p->shown.width : 0;
    surface->height = p->has_frame ? p->shown.height : 0;
    surface->format = p->has_frame ? p->shown.format : MEDIA_RGBA;
    unsigned pane = overlay->chat ? PLAYER_CHAT_EDGE : DISPLAY_WIDTH;
    unsigned draw_width = overlay->chat ? 896 : DISPLAY_WIDTH;
    unsigned available_height = overlay->chat ? 504 : DISPLAY_HEIGHT;
    unsigned draw_height = 0;
    if (surface->width && surface->height) {
        draw_height = surface->height * draw_width / surface->width;
        if (draw_height > available_height) {
            draw_height = available_height;
            draw_width = surface->width * draw_height / surface->height;
        }
        if (!draw_width) draw_width = 1;
        if (!draw_height) draw_height = 1;
    }
    unsigned left = (pane - draw_width) / 2, top = (DISPLAY_HEIGHT - draw_height) / 2;
    surface->left = ui_px(left);
    surface->top = ui_px(top);
    surface->draw_width = ui_px(left + draw_width) - surface->left;
    surface->draw_height = ui_px(top + draw_height) - surface->top;

    bool controls = overlay->chrome || overlay->paused || overlay->page != PLAYER_CONTROLS;
    unsigned position_tick = controls ? overlay->position_ms / 250 : 0;
    const char *caption = overlay->captions_on ? overlay->caption : "";
    const char *track = overlay->caption_track;
    if (redraw || layout_changed || display->position_tick != position_tick ||
        display->duration != overlay->duration_ms || display->quality != overlay->quality ||
        display->chat_failed != overlay->chat_failed || display->chat_can_send != overlay->chat_can_send ||
        display->caption_seen != overlay->caption_seen ||
        strcmp(display->caption, caption) || strcmp(display->caption_track, track)) {
        unsigned pixel_width = ui_px(pane);
        if (pixel_width == UI_WIDTH)
            memset(display->pixels, 0, UI_WIDTH * UI_HEIGHT * sizeof(uint32_t));
        else for (unsigned y = 0; y < UI_HEIGHT; ++y)
            memset(display->pixels + y * UI_WIDTH, 0, pixel_width * sizeof(uint32_t));
        render_player_layer(display->pixels, UI_WIDTH, overlay);
        display->position_tick = position_tick;
        display->duration = overlay->duration_ms;
        display->quality = overlay->quality;
        display->chat_failed = overlay->chat_failed;
        display->chat_can_send = overlay->chat_can_send;
        display->caption_seen = overlay->caption_seen;
        snprintf(display->caption, sizeof(display->caption), "%s", caption);
        snprintf(display->caption_track, sizeof(display->caption_track), "%s", track);
    }
    if (overlay->chat && (chat_dirty || layout_changed)) {
        for (unsigned y = 0; y < UI_HEIGHT; ++y)
            memcpy(display->pixels + y * UI_WIDTH + UI_CHAT_EDGE,
                   chat_pixels + y * UI_CHAT_WIDTH, UI_CHAT_WIDTH * sizeof(uint32_t));
    }
    return true;
}
#else
static bool draw_video(PlayerDisplay *display, Playback *p, const PlayerOverlay *overlay,
                       bool redraw, bool chat_dirty) {
    if (!take_latest(p) && !redraw && !chat_dirty) return true;
    u32 stride;
    uint8_t *target = display_begin(display, &stride);
    if (!target) return true;

    if (overlay->chat) {
        for (unsigned y = 0; y < DISPLAY_HEIGHT; ++y) {
            uint32_t *dst = (void *)(target + (size_t)y * stride);
            memcpy(dst + PLAYER_CHAT_EDGE, chat_pixels + (size_t)y * PLAYER_CHAT_WIDTH,
                   PLAYER_CHAT_WIDTH * sizeof(*chat_pixels));
        }
    }
    unsigned pane = overlay->chat ? PLAYER_CHAT_EDGE : DISPLAY_WIDTH;
    if (!p->has_frame) {
        for (unsigned y = 0; y < DISPLAY_HEIGHT; ++y) {
            uint32_t *dst = (void *)(target + (size_t)y * stride);
            for (unsigned x = 0; x < pane; ++x) dst[x] = UINT32_C(0xff171119);
        }
        render_player_overlay((uint32_t *)target, stride / 4, overlay);
        display_end(display);
        return true;
    }

    unsigned width = p->shown.width, height = p->shown.height;
    unsigned available_w = overlay->chat ? 896 : DISPLAY_WIDTH;
    unsigned available_h = overlay->chat ? 504 : DISPLAY_HEIGHT;
    unsigned draw_w = available_w, draw_h = width ? height * draw_w / width : 0;
    if (draw_h > available_h) { draw_h = available_h; draw_w = width * draw_h / height; }
    if (!draw_w) draw_w = 1;
    if (!draw_h) draw_h = 1;
    unsigned left = (pane - draw_w) / 2, top = (DISPLAY_HEIGHT - draw_h) / 2;
    if (left || top || draw_w != pane || draw_h != DISPLAY_HEIGHT) {
        for (unsigned y = 0; y < DISPLAY_HEIGHT; ++y) {
            uint32_t *dst = (void *)(target + (size_t)y * stride);
            if (y < top || y >= top + draw_h) {
                for (unsigned x = 0; x < pane; ++x) dst[x] = UINT32_C(0xff171419);
            } else {
                for (unsigned x = 0; x < left; ++x) dst[x] = UINT32_C(0xff171419);
                for (unsigned x = left + draw_w; x < pane; ++x) dst[x] = UINT32_C(0xff171419);
            }
        }
    }
    unsigned source_x[DISPLAY_WIDTH];
    if (draw_w != width && draw_w != width * 2)
        for (unsigned x = 0; x < draw_w; ++x) source_x[x] = x * width / draw_w;
    for (unsigned y = 0; y < draw_h; ++y) {
        uint8_t *dst = target + (size_t)(top + y) * stride + left * 4;
        const uint8_t *src = p->shown.rgba + (size_t)(y * height / draw_h) * width * 4;
        if (draw_w == width) memcpy(dst, src, (size_t)width * 4);
        else if (draw_w == width * 2) {
            for (unsigned x = 0; x < width; ++x) {
                uint32_t pixel;
                memcpy(&pixel, src + x * 4, 4);
                uint64_t pair = (uint64_t)pixel << 32 | pixel;
                memcpy(dst + x * 8, &pair, 8);
            }
        } else {
            for (unsigned x = 0; x < draw_w; ++x)
                memcpy(dst + x * 4, src + source_x[x] * 4, 4);
        }
    }
    render_player_overlay((uint32_t *)target, stride / 4, overlay);
    display_end(display);
    return true;
}
#endif

static bool keyboard(const char *guide, const char *initial, char *input, size_t capacity) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
    swkbdConfigMakePresetDefault(&kbd);
#ifndef IZTWINS2_IZA
    swkbdConfigSetType(&kbd, SwkbdType_QWERTY);
#endif
    swkbdConfigSetGuideText(&kbd, guide);
    swkbdConfigSetInitialText(&kbd, initial);
#ifndef IZTWINS2_IZA
    swkbdConfigSetStringLenMax(&kbd, capacity - 1);
#endif
    Result rc = swkbdShow(&kbd, input, capacity);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc);
}

#ifdef IZTWINS2_IZA
static const char library_path[] = "library.txt";
static const char session_path[] = "session.txt";
#else
static const char library_path[] = "sdmc:/switch/IZtwiNS2/library.txt";
static const char session_path[] = "sdmc:/switch/IZtwiNS2/session.txt";
static const char exit_log_path[] = "sdmc:/switch/IZtwiNS2/exit.log";
static const char launcher_path[] = "sdmc:/switch/IZ2S/IZ2S.nro";
static const char launcher_companion[] = "sdmc:/iz2s/modules/iztwins2/return-iz2s.nro";
#endif

enum { CATALOG_PAGES = 100 };
typedef struct {
    unsigned index, revision;
    char cursor[CATALOG_PAGES][CATALOG_CURSOR_BYTES];
} CatalogPage;

typedef struct {
    bool valid, history, touch_input;
    unsigned revision;
    ChatScroll scroll;
    TextSize size;
    char notice[96];
} ChatPanel;

typedef struct {
    Playback *playback;
    Audio audio;
    TwitchAccount account;
    AccountJob job;
    PreviewCache previews;
    TwitchChat chat;
    ChatSnapshot chat_view;
    ChatPanel chat_panel;
    TouchLayout menu_touch, player_touch, chat_touch;
    bool touch_input;
    unsigned playback_generation;
    ChannelLibrary library;
    ChannelList following, popular, trends, search, filtered, category, channel_live, filtered_clips;
    ChannelProfile profile;
    MenuTab tab, root_tab, back[8];
    unsigned back_count;
    SearchFilter search_filter;
    MenuFocus menu_focus;
    unsigned rail_choice, tool_choice, selector_choice, pager_choice;
    unsigned selected[MENU_TAB_COUNT];
    bool loaded[MENU_TAB_COUNT], dirty, confirm_sign_out, ignore_job, seen_device_code, watched;
    bool browse_pending, sign_in_pending;
    int page_step;
    CatalogPage pages[MENU_TAB_COUNT];
    bool remember_session, confirm_remember;
#ifdef IZTWINS2_IZA
    bool library_pending;
#endif
    bool own_sd, have_sd, restore_playing;
    bool chat_visible;
    bool returning_to_launcher;
    unsigned chat_revision, last_video_index;
    Channel playing;
    unsigned player_choice, player_focus, failed_position, seek_ms, busy_phase;
    unsigned restart_position, fallback_quality, recoveries;
    PlayerPage player_page;
    PlayerSetting player_setting;
    bool player_chrome, player_muted, playback_error, playback_closing, pause_when_ready, restart_pending;
    const char *buffering_text, *shown_busy;
    const char *exit_detail;
    uint64_t chrome_until, next_chat_poll, next_position_save, next_favorites_refresh, playback_since;
    ChatScroll chat_scroll;
    uint64_t last_video_at;
    uint64_t retry_check_at;
    char query[96], last_channel[CHANNEL_LOGIN_BYTES], status[160];
    char category_title[96], profile_title[96], playing_title[224], chat_notice[96];
    char category_id[96], profile_login[CHANNEL_LOGIN_BYTES];
    char clip_filter[96];
} App;

static ChannelList *active_list(App *app) {
    switch (app->tab) {
        case FOLLOWING: return &app->following;
        case POPULAR: return &app->popular;
        case FAVORITES: return &app->library.favorites;
        case RECENT: return &app->library.recent;
        case CONTINUE_WATCHING: return &app->library.resume;
        case TRENDS: return &app->trends;
        case SEARCH: return app->search_filter == SEARCH_ALL ? &app->search : &app->filtered;
        case CATEGORY: return &app->category;
        case CHANNEL_LIVE: return &app->channel_live;
        case CHANNEL_VIDEOS: return &app->profile.videos;
        case CHANNEL_CLIPS: return *app->clip_filter ? &app->filtered_clips : &app->profile.clips;
        case MENU_TAB_COUNT: break;
    }
    return &app->following;
}

static const Channel *focused_channel(App *app) {
    ChannelList *list = active_list(app);
    unsigned *selected = &app->selected[app->tab];
    if (*selected >= list->count) *selected = list->count ? list->count - 1 : 0;
    return list->count ? &list->items[*selected] : NULL;
}

static void focus_card(App *app, const Channel *selected) {
    ChannelList *list = active_list(app);
    bool by_login = selected->kind == ITEM_CHANNEL || selected->kind == ITEM_LIVE;
    for (unsigned i = 0; i < list->count; ++i) {
        const Channel *item = &list->items[i];
        bool same = by_login ?
            (item->kind == ITEM_CHANNEL || item->kind == ITEM_LIVE) && *selected->login && !strcmp(item->login, selected->login) :
            item->kind == selected->kind && *selected->id && !strcmp(item->id, selected->id);
        if (same) { app->selected[app->tab] = i; return; }
    }
}

static void restore_played_card(App *app) {
    if (app->restore_playing) focus_card(app, &app->playing);
}

static void set_status(App *app, const char *message) {
    snprintf(app->status, sizeof(app->status), "%s", message);
    app->dirty = true;
}

static void save_library(App *app) {
#ifdef IZTWINS2_IZA
    if (app->have_sd && app->playback && app->playback->kind == ITEM_LIVE) {
        app->library_pending = true;
        return;
    }
    app->library_pending = false;
#endif
    if (!app->have_sd || !library_save(&app->library, library_path))
        set_status(app, "SD card unavailable. Changes last until you exit.");
}

static void save_session(App *app, const TwitchAccount *account) {
    if (!session_save(account, session_path)) {
        app->remember_session = false;
        session_remove(session_path);
        set_status(app, "The SD card could not save your Twitch session.");
    }
}

static void save_position(App *app, bool finished) {
    Playback *p = app->playback;
    if (!p || app->playing.kind != ITEM_VIDEO || !app->watched) return;
    bool changed = finished ? library_remove_resume(&app->library, app->playing.id) :
        library_set_resume(&app->library, &app->playing, atomic_load(&p->position_ms), atomic_load(&p->duration_ms));
    if (changed) save_library(app);
}

static void filter_search(App *app) {
    app->filtered.count = 0;
    for (unsigned i = 0; i < app->search.count; ++i) {
        Channel *item = &app->search.items[i];
        if ((app->search_filter == SEARCH_LIVE && item->kind == ITEM_LIVE) ||
            (app->search_filter == SEARCH_CHANNELS && (item->kind == ITEM_CHANNEL || item->kind == ITEM_LIVE)) ||
            (app->search_filter == SEARCH_CATEGORIES && item->kind == ITEM_CATEGORY) ||
            (app->search_filter == SEARCH_VIDEOS && item->kind == ITEM_VIDEO))
            app->filtered.items[app->filtered.count++] = *item;
    }
}

static void filter_clips(App *app) {
    ChannelList *out = &app->filtered_clips;
    const ChannelList *source = &app->profile.clips;
    out->count = 0;
    strcpy(out->next, source->next);
    if (!*app->clip_filter) return;
    size_t length = strlen(app->clip_filter);
    for (unsigned i = 0; i < source->count; ++i) {
        const Channel *clip = &source->items[i];
        for (const char *at = clip->title; *at; ++at) {
            if (strncasecmp(at, app->clip_filter, length)) continue;
            out->items[out->count++] = *clip;
            break;
        }
    }
}

static MenuTab catalog_tab(AccountAction action) {
    switch (action) {
        case ACCOUNT_FOLLOWING: return FOLLOWING;
        case ACCOUNT_POPULAR: return POPULAR;
        case ACCOUNT_TRENDS: return TRENDS;
        case ACCOUNT_SEARCH: return SEARCH;
        case ACCOUNT_CATEGORY: return CATEGORY;
        case ACCOUNT_CHANNEL: return CHANNEL_LIVE;
        case ACCOUNT_VIDEOS: return CHANNEL_VIDEOS;
        case ACCOUNT_CLIPS: return CHANNEL_CLIPS;
        case ACCOUNT_FAVORITES: return FAVORITES;
        default: return MENU_TAB_COUNT;
    }
}

static void reset_page(App *app, MenuTab tab) {
    app->pages[tab].index = 0;
    ++app->pages[tab].revision;
}

static bool begin_page_job(App *app, AccountAction action, const char *key, const char *cursor, int step) {
    if (app->job.running) return false;
    app->ignore_job = app->seen_device_code = false;
    if (account_start(&app->job, action, &app->account, key, cursor)) {
        app->job.page_step = step;
        MenuTab tab = catalog_tab(action);
        app->job.route_revision = tab == MENU_TAB_COUNT ? 0 : app->pages[tab].revision;
        app->job.from_page = tab == MENU_TAB_COUNT ? 0 : app->pages[tab].index;
        if (action != ACCOUNT_CHECK) set_status(app, "");
        return true;
    }
    set_status(app, "Could not start the Twitch request.");
    return false;
}

static bool begin_job(App *app, AccountAction action, const char *key) {
    return begin_page_job(app, action, key, NULL, 0);
}

static void refresh_favorites(App *app, uint64_t now) {
    const ChannelList *list = app->tab == RECENT ? &app->library.recent : &app->library.favorites;
    if (!list->count || app->job.running) return;
    char names[FAVORITE_QUERY_BYTES];
    size_t written = 0;
    for (unsigned i = 0; i < list->count; ++i) {
        const char *login = list->items[i].login;
        size_t length = strlen(login);
        if (i) names[written++] = ',';
        memcpy(names + written, login, length);
        written += length;
    }
    names[written] = 0;
    bool started = begin_job(app, ACCOUNT_FAVORITES, names);
    app->next_favorites_refresh = now + (started ? UINT64_C(60000000000) : UINT64_C(15000000000));
}

static bool catalog_action(MenuTab tab, AccountAction *action) {
    switch (tab) {
        case FOLLOWING: *action = ACCOUNT_FOLLOWING; return true;
        case POPULAR: *action = ACCOUNT_POPULAR; return true;
        case TRENDS: *action = ACCOUNT_TRENDS; return true;
        case FAVORITES: case RECENT: *action = ACCOUNT_FAVORITES; return true;
        case SEARCH: *action = ACCOUNT_SEARCH; return true;
        case CATEGORY: *action = ACCOUNT_CATEGORY; return true;
        case CHANNEL_LIVE: *action = ACCOUNT_CHANNEL; return true;
        case CHANNEL_VIDEOS: *action = ACCOUNT_VIDEOS; return true;
        case CHANNEL_CLIPS: *action = ACCOUNT_CLIPS; return true;
        default: return false;
    }
}

static const char *catalog_key(const App *app, AccountAction action) {
    switch (action) {
        case ACCOUNT_SEARCH: return app->query;
        case ACCOUNT_CATEGORY: return app->category_id;
        case ACCOUNT_CHANNEL: case ACCOUNT_VIDEOS: case ACCOUNT_CLIPS: return app->profile_login;
        default: return NULL;
    }
}

static bool catalog_matches(const App *app, const AccountJob *job) {
    MenuTab tab = catalog_tab(job->action);
    if (tab != MENU_TAB_COUNT && (app->pages[tab].revision != job->route_revision ||
        app->pages[tab].index != job->from_page)) return false;
    const char *key = catalog_key(app, job->action);
    return !key || !strcmp(key, job->query);
}

static bool catalog_visible(const App *app, const AccountJob *job) {
    AccountAction action;
    if (!catalog_matches(app, job)) return false;
    if (job->action == ACCOUNT_CHANNEL && menu_profile_tab(app->tab) && !app->pages[app->tab].index) return true;
    return catalog_action(app->tab, &action) && job->action == action;
}

static bool can_page(App *app, int step) {
    CatalogPage *page = &app->pages[app->tab];
    if (step < 0) return page->index != 0;
    const char *next = active_list(app)->next;
    return page->index + 1 < CATALOG_PAGES && *next && strcmp(next, page->cursor[page->index]);
}

static void load_tab(App *app, bool refresh) {
    app->browse_pending = false;
    if (!refresh) app->page_step = 0;
    AccountAction action;
    if (!catalog_action(app->tab, &action)) return;
    if (menu_profile_tab(app->tab) && !app->loaded[CHANNEL_LIVE]) action = ACCOUNT_CHANNEL;
    const char *key = catalog_key(app, action);
    if (key && !*key) return;
    if (action == ACCOUNT_FOLLOWING && !*app->account.access && !*app->account.refresh) return;
    uint64_t now = now_ns();
    if (action == ACCOUNT_FAVORITES) {
        if (!active_list(app)->count || (!refresh && now < app->next_favorites_refresh)) return;
    } else {
        MenuTab scope = menu_profile_tab(app->tab) ? CHANNEL_LIVE : app->tab;
        if (!refresh && app->loaded[scope]) return;
    }
    int step = app->page_step;
    if (step && !can_page(app, step)) step = app->page_step = 0;
    const CatalogPage *page = &app->pages[app->tab];
    const char *cursor = step > 0 ? active_list(app)->next : page->cursor[page->index - (step < 0)];
    if (app->job.running) {
        bool same = catalog_visible(app, &app->job) && !strcmp(cursor, app->job.cursor);
        app->browse_pending = !same || atomic_load(&app->job.cancelled);
        if (!app->browse_pending) app->page_step = 0;
        app->dirty = true;
    } else if (action == ACCOUNT_FAVORITES) refresh_favorites(app, now);
    else {
        app->page_step = 0;
        begin_page_job(app, action, key, cursor, step);
    }
}

static void clear_remote(App *app) {
    app->following.count = 0;
    app->following.next[0] = 0;
    reset_page(app, FOLLOWING);
    app->loaded[FOLLOWING] = false;
}

static void poll_job(App *app) {
    AccountJob *job = &app->job;
    if (!job->running) return;
    if (!app->seen_device_code && atomic_load(&job->code_ready)) {
        app->seen_device_code = true;
        app->dirty = true;
    }
    if (!atomic_load(&job->finished)) return;
    account_join(job);
    app->dirty = true;
    if (app->ignore_job || atomic_load(&job->cancelled)) {
        account_clear(&job->account);
        return;
    }

    bool had_session = *app->account.access || *app->account.refresh;
    bool restoring = !*app->account.access && *app->account.refresh;
    bool changed_access = strcmp(app->account.access, job->account.access) != 0;
    bool changed_refresh = strcmp(app->account.refresh, job->account.refresh) != 0;
    account_clear(&app->account);
    app->account = job->account;
    account_clear(&job->account);
    bool lost_session = had_session && !*app->account.access && !*app->account.refresh;
    if (lost_session) {
        clear_remote(app);
        if (app->remember_session) session_remove(session_path);
        app->remember_session = false;
    } else if (app->remember_session && changed_refresh && *app->account.refresh) save_session(app, &app->account);
    if (changed_access && app->playback && app->playback->kind == ITEM_LIVE && app->chat.running) {
        chat_request_stop(&app->chat);
        app->chat_scroll = (ChatScroll){0};
    }
    bool visible = catalog_visible(app, job);
    if (!job->success) {
        if (job->action == ACCOUNT_CHECK || job->action == ACCOUNT_FOLLOWING)
            app->retry_check_at = now_ns() + UINT64_C(30000000000);
        if (visible || job->action == ACCOUNT_SIGN_IN || lost_session || (restoring && app->tab == FOLLOWING))
            set_status(app, *job->status ? job->status : "The Twitch request failed.");
        return;
    }
    if (!catalog_matches(app, job)) return;
    Channel selection = {0};
    const Channel *row = visible && !job->page_step ? focused_channel(app) : NULL;
    if (row) selection = *row;
    switch (job->action) {
        case ACCOUNT_SIGN_IN:
            clear_remote(app);
            app->tab = app->root_tab = FOLLOWING;
            app->menu_focus = MENU_GRID;
            app->back_count = 0;
            app->page_step = 0;
            app->selected[FOLLOWING] = 0;
            if (app->have_sd && *app->account.refresh) {
                app->confirm_remember = true;
                app->browse_pending = true;
            } else load_tab(app, true);
            break;
        case ACCOUNT_FOLLOWING:
            app->following = job->rows;
            app->loaded[FOLLOWING] = true;
            break;
        case ACCOUNT_POPULAR:
            app->popular = job->rows;
            app->loaded[POPULAR] = true;
            break;
        case ACCOUNT_TRENDS:
            app->trends = job->rows;
            app->loaded[TRENDS] = true;
            break;
        case ACCOUNT_SEARCH:
            app->search = job->rows;
            app->loaded[SEARCH] = true;
            filter_search(app);
            break;
        case ACCOUNT_CATEGORY:
            app->category = job->rows;
            app->loaded[CATEGORY] = true;
            break;
        case ACCOUNT_CHANNEL:
            app->profile.header = job->profile.header;
            memcpy(app->profile.description, job->profile.description, sizeof(app->profile.description));
            if (!app->pages[CHANNEL_VIDEOS].index) app->profile.videos = job->profile.videos;
            if (!app->pages[CHANNEL_CLIPS].index) {
                app->profile.clips = job->profile.clips;
                filter_clips(app);
            }
            app->channel_live.count = app->profile.header.kind == ITEM_LIVE ? 1 : 0;
            if (app->channel_live.count) app->channel_live.items[0] = app->profile.header;
            app->loaded[CHANNEL_LIVE] = true;
            break;
        case ACCOUNT_VIDEOS:
            app->profile.videos = job->rows;
            break;
        case ACCOUNT_CLIPS:
            app->profile.clips = job->rows;
            filter_clips(app);
            break;
        case ACCOUNT_FAVORITES: {
            ChannelList *lists[] = {&app->library.favorites, &app->library.recent};
            for (unsigned list = 0; list < sizeof(lists) / sizeof(lists[0]); ++list)
                for (unsigned i = 0; i < lists[list]->count; ++i) {
                    Channel *saved = &lists[list]->items[i];
                    for (unsigned j = 0; j < job->rows.count; ++j) {
                        if (strcmp(saved->login, job->rows.items[j].login)) continue;
                        *saved = job->rows.items[j];
                        break;
                    }
                }
            break;
        }
        case ACCOUNT_CHECK:
            if (restoring && app->tab == FOLLOWING && !app->playback) load_tab(app, false);
            break;
    }
    MenuTab page_tab = catalog_tab(job->action);
    if (job->page_step && page_tab != MENU_TAB_COUNT) {
        CatalogPage *page = &app->pages[page_tab];
        page->index = (unsigned)((int)page->index + job->page_step);
        snprintf(page->cursor[page->index], sizeof(page->cursor[page->index]), "%s", job->cursor);
        app->selected[page_tab] = 0;
        if (visible) app->menu_focus = MENU_GRID;
    }
    if (row) focus_card(app, &selection);
}

static void load_pending(App *app, uint64_t now) {
    if (app->job.running || app->playback || app->playback_error || app->confirm_sign_out || app->confirm_remember) return;
    if (app->sign_in_pending) {
        app->sign_in_pending = false;
        begin_job(app, ACCOUNT_SIGN_IN, NULL);
    } else if (app->browse_pending) load_tab(app, true);
    else if ((app->tab == FAVORITES || app->tab == RECENT) && now >= app->next_favorites_refresh)
        refresh_favorites(app, now);
}

static void draw_menu(PlayerDisplay *display, App *app) {
#ifdef IZTWINS2_IZA
    display->surface->playback = false;
#endif
    u32 stride;
    uint32_t *pixels = display_begin(display, &stride);
    if (!pixels) return;
    focused_channel(app);
    const AccountJob *job = &app->job;
    bool signing = app->sign_in_pending || (job->running && job->action == ACCOUNT_SIGN_IN);
    bool profile = menu_profile_tab(app->tab);
    char profile_note[464];
    if (profile) snprintf(profile_note, sizeof(profile_note), "%s%s%s",
        *app->profile.header.login ? (app->channel_live.count ? "Live now" : "Offline") : "Channel",
        *app->profile.description ? " · " : "", app->profile.description);
    MenuFrame frame = {
        .tab = app->tab, .root_tab = app->root_tab, .focus = app->menu_focus,
        .touch = &app->menu_touch, .touch_input = app->touch_input,
        .rail = app->rail_choice, .tool = app->tool_choice,
        .page_number = app->pages[app->tab].index + 1, .pager_choice = app->pager_choice,
        .previous_page = can_page(app, -1), .next_page = can_page(app, 1),
        .selector = profile ? (unsigned)(app->tab - CHANNEL_LIVE) : (unsigned)app->search_filter,
        .selector_hover = app->selector_choice,
        .list = active_list(app), .selected = app->selected[app->tab], .query = app->query,
        .login = app->account.login, .status = app->status,
        .device_code = signing && !app->sign_in_pending && app->seen_device_code ? job->code : "",
        .activation_link = signing && !app->sign_in_pending && app->seen_device_code ? job->activation_link : "",
        .playback_title = app->playing_title,
        .playback_error = app->playback_error,
        .heading = profile ? app->profile_title : app->tab == CATEGORY ? app->category_title : NULL,
        .channel_note = profile ? profile_note : NULL,
        .clip_filter = app->clip_filter,
        .exit_detail = app->exit_detail, .returning_to_launcher = app->returning_to_launcher,
        .loading = app->browse_pending || (job->running && catalog_visible(app, job)),
        .signing_in = signing, .confirm_sign_out = app->confirm_sign_out,
        .confirm_remember = app->confirm_remember, .restoring_session = !*app->account.access && *app->account.refresh,
        .cancel_pending = signing && !app->sign_in_pending && atomic_load(&job->cancelled),
    };
    unsigned first = menu_page(app->tab, frame.selected);
    for (unsigned i = 0; i < menu_page_size(app->tab, frame.selected) && first + i < frame.list->count; ++i) {
        const Channel *row = &frame.list->items[first + i];
        frame.previews[i] = previews_find(&app->previews, row->thumbnail);
        frame.saved[i] = library_contains(&app->library.favorites, row->login);
        const Channel *bookmark = row->kind == ITEM_VIDEO ? library_resume(&app->library, row->id) : NULL;
        frame.progress[i] = bookmark ? bookmark->resume_ms : 0;
        frame.duration[i] = bookmark ? bookmark->duration_ms : row->duration_ms;
    }
    render_menu(pixels, stride / 4, &frame);
    display_end(display);
    app->dirty = false;
}

static void open_playback(App *app, unsigned resume_ms) {
    ++app->playback_generation;
    app->player_touch.count = app->chat_touch.count = 0;
    app->chat_panel.valid = false;
    app->audio = (Audio){0};
    app->watched = false;
    app->player_page = PLAYER_CONTROLS;
    app->player_chrome = false;
    app->playback_error = false;
    app->playback_closing = app->pause_when_ready = app->restart_pending = false;
    app->playback_since = 0;
    app->last_video_at = 0;
    app->last_video_index = 0;
    app->shown_busy = NULL;
    app->buffering_text = "Connecting to Twitch…";
    app->busy_phase = 8;
    app->failed_position = resume_ms;
    app->chrome_until = 0;
    app->next_position_save = now_ns() + UINT64_C(15000000000);
    if (!audio_open(&app->audio)) {
        set_status(app, "Could not initialize audio.");
        app->playback_error = true;
    }
    else if (!(app->playback = start(&app->playing,
            app->library.preferred_quality ? app->library.preferred_quality : app->fallback_quality,
            resume_ms, app->library.captions_on))) {
        set_status(app, "Could not start the player.");
        app->playback_error = true;
        audio_close(&app->audio);
    } else {
        if (app->player_muted && R_FAILED(audoutSetAudioOutVolume(0))) app->player_muted = false;
        if (app->playing.kind == ITEM_LIVE && app->chat_visible)
            chat_start(&app->chat, app->playback->channel, &app->account);
        if (app->previews.running) atomic_store(&app->previews.cancelled, true);
        set_status(app, "");
    }
}

static void play_item(App *app, const Channel *item) {
    app->restore_playing = app->menu_focus == MENU_GRID && item == focused_channel(app);
    app->playing = *item;
    app->fallback_quality = app->recoveries = 0;
    if (item->kind == ITEM_LIVE) snprintf(app->last_channel, sizeof(app->last_channel), "%s", item->login);
    const char *name = *item->title ? item->title : *item->login ? item->login : item->id;
    snprintf(app->playing_title, sizeof(app->playing_title), "%s", name);
    app->chat_visible = false;
    app->chat_scroll = (ChatScroll){0};
    memset(&app->chat_view, 0, sizeof(app->chat_view));
    app->chat_revision = 0;
    app->chat_panel.valid = false;
    app->next_chat_poll = 0;
    app->player_focus = 0;
    app->chat_notice[0] = 0;
    const Channel *bookmark = item->kind == ITEM_VIDEO ? library_resume(&app->library, item->id) : NULL;
    open_playback(app, bookmark ? bookmark->resume_ms : 0);
}

static void reopen_playback(App *app, unsigned at_ms, const char *reason) {
    app->restart_pending = true;
    app->restart_position = at_ms;
    app->buffering_text = reason;
    app->player_page = PLAYER_CONTROLS;
    audio_close(&app->audio);
    atomic_store(&app->playback->stop, true);
    atomic_store(&app->playback->subtitle_stop, true);
}

static void finish_restart(App *app) {
    Playback *old = app->playback;
    PlaybackFrame previous = old->shown;
    old->shown = (PlaybackFrame){0};
    bool paused = atomic_load(&old->paused) || app->pause_when_ready;
    const char *reason = app->buffering_text;
    save_position(app, false);
    clear(old);
    app->playback = NULL;
    open_playback(app, app->restart_position);
    if (app->playback) {
        app->playback->shown = previous;
        app->playback->has_frame = previous.rgba != NULL;
        app->pause_when_ready = paused;
        app->buffering_text = reason;
    } else {
        free(previous.rgba);
        chat_request_stop(&app->chat);
    }
}

static unsigned lower_quality(const Playback *p, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        if (p->qualities[i] < p->playing_quality) return p->qualities[i];
    return p->preferred_quality;
}

static void direct_channel(App *app) {
    char input[192] = "";
    if (!keyboard("Twitch channel name or link", app->last_channel, input, sizeof(input))) return;
    const char prefix[] = "https://www.twitch.tv/";
    const char short_prefix[] = "https://twitch.tv/";
    const char clip_prefix[] = "https://clips.twitch.tv/";
    char *login = input;
    Channel item = {0};
    if (!strncmp(login, clip_prefix, sizeof(clip_prefix) - 1)) {
        login += sizeof(clip_prefix) - 1;
        login[strcspn(login, "/?#")] = 0;
        if (!twitch_clip_id(login)) { set_status(app, "Invalid Twitch clip link."); return; }
        item.kind = ITEM_CLIP;
        memcpy(item.id, login, strlen(login) + 1);
        play_item(app, &item);
        return;
    }
    if (!strncmp(login, prefix, sizeof(prefix) - 1)) login += sizeof(prefix) - 1;
    else if (!strncmp(login, short_prefix, sizeof(short_prefix) - 1)) login += sizeof(short_prefix) - 1;
    if (login != input) {
        char *id = NULL;
        if (!strncmp(login, "videos/", 7)) { id = login + 7; item.kind = ITEM_VIDEO; }
        else {
            char *clip = strchr(login, '/');
            if (clip && !strncmp(clip, "/clip/", 6)) { id = clip + 6; item.kind = ITEM_CLIP; }
        }
        if (id) {
            id[strcspn(id, "/?#")] = 0;
            if (!(item.kind == ITEM_VIDEO ? twitch_video_id(id) : twitch_clip_id(id))) {
                set_status(app, "Invalid Twitch video link."); return;
            }
            memcpy(item.id, id, strlen(id) + 1);
            play_item(app, &item);
            return;
        }
        login[strcspn(login, "/?#")] = 0;
    }
    if (!twitch_channel(login)) set_status(app, "Enter a Twitch channel name or a channel, video, or clip link.");
    else { memcpy(item.login, login, strlen(login) + 1); play_item(app, &item); }
}

static bool same_view(MenuTab a, MenuTab b) {
    return a == b || (menu_profile_tab(a) && menu_profile_tab(b));
}

static void enter_view(App *app, MenuTab destination) {
    if (app->tab == destination) return;
    unsigned retained = 0;
    for (unsigned i = 0; i < app->back_count; ++i)
        if (!same_view(app->back[i], destination)) app->back[retained++] = app->back[i];
    app->back_count = retained;
    const unsigned limit = sizeof(app->back) / sizeof(*app->back);
    if (app->back_count == limit) {
        memmove(app->back, app->back + 1, (limit - 1) * sizeof(*app->back));
        --app->back_count;
    }
    app->back[app->back_count++] = app->tab;
    app->tab = destination;
    app->menu_focus = MENU_GRID;
}

static void search_channels(App *app) {
    char input[sizeof(app->query)] = "";
    if (!keyboard("Search Twitch channels, categories and videos", app->query, input, sizeof(input)) || !*input) return;
    snprintf(app->query, sizeof(app->query), "%s", input);
    enter_view(app, SEARCH);
    app->menu_focus = MENU_GRID;
    app->search_filter = SEARCH_ALL;
    reset_page(app, SEARCH);
    app->page_step = 0;
    app->filtered.count = app->search.count = app->selected[SEARCH] = 0;
    app->loaded[SEARCH] = false;
    set_status(app, "");
    load_tab(app, true);
}

static void search_clips(App *app) {
    char input[sizeof(app->clip_filter)] = "";
    if (!keyboard("Filter clip titles. Clear text to show all.", app->clip_filter, input, sizeof(input))) return;
    snprintf(app->clip_filter, sizeof(app->clip_filter), "%s", input);
    filter_clips(app);
    app->selected[CHANNEL_CLIPS] = 0;
    app->menu_focus = MENU_GRID;
    set_status(app, "");
}

static void open_category(App *app, const Channel *row) {
    snprintf(app->category_title, sizeof(app->category_title), "%s", row->name);
    snprintf(app->category_id, sizeof(app->category_id), "%s", row->id);
    enter_view(app, CATEGORY);
    app->category.count = app->selected[CATEGORY] = 0;
    app->category.next[0] = 0;
    reset_page(app, CATEGORY);
    app->page_step = 0;
    app->loaded[CATEGORY] = false;
    set_status(app, "");
    load_tab(app, true);
}

static void open_profile(App *app, const Channel *row) {
    if (!*row->login || menu_profile_tab(app->tab)) return;
    snprintf(app->profile_title, sizeof(app->profile_title), "%s", *row->name ? row->name : row->login);
    snprintf(app->profile_login, sizeof(app->profile_login), "%s", row->login);
    enter_view(app, row->kind == ITEM_LIVE ? CHANNEL_LIVE : CHANNEL_VIDEOS);
    app->channel_live.count = app->selected[CHANNEL_LIVE] = 0;
    app->profile.header = (Channel){0};
    app->profile.description[0] = 0;
    app->profile.videos.count = app->profile.clips.count = 0;
    app->profile.videos.next[0] = app->profile.clips.next[0] = app->clip_filter[0] = 0;
    reset_page(app, CHANNEL_LIVE);
    reset_page(app, CHANNEL_VIDEOS);
    reset_page(app, CHANNEL_CLIPS);
    app->page_step = 0;
    app->selected[CHANNEL_VIDEOS] = app->selected[CHANNEL_CLIPS] = 0;
    app->loaded[CHANNEL_LIVE] = false;
    set_status(app, "");
    load_tab(app, true);
}

static unsigned rail_index(MenuTab tab) {
    for (unsigned i = 0; i < MENU_ROOT_TABS; ++i)
        if (menu_root_tabs[i] == tab) return i;
    return 0;
}

static void select_section(App *app, unsigned index) {
    app->menu_focus = MENU_GRID;
    app->tab = app->root_tab = menu_root_tabs[index];
    app->back_count = 0;
    if (app->tab == FAVORITES || app->tab == RECENT) app->next_favorites_refresh = 0;
    set_status(app, "");
    load_tab(app, false);
}

static void account_menu(App *app) {
    if (*app->account.access || *app->account.refresh) { app->confirm_sign_out = true; app->dirty = true; }
    else if (app->job.running) { app->sign_in_pending = true; app->dirty = true; }
    else begin_job(app, ACCOUNT_SIGN_IN, NULL);
}

static void link_menu(App *app) { direct_channel(app); }

static unsigned browse_selectors(MenuTab tab) {
    if (tab == SEARCH) return SEARCH_FILTERS;
    return menu_profile_tab(tab) ? 3 : 0;
}

static unsigned browse_selector(const App *app) {
    return app->tab == SEARCH ? (unsigned)app->search_filter : (unsigned)(app->tab - CHANNEL_LIVE);
}

static void choose_selector(App *app, unsigned which) {
    if (app->tab == SEARCH) {
        if ((unsigned)app->search_filter != which) {
            app->search_filter = (SearchFilter)which;
            app->selected[SEARCH] = 0;
            filter_search(app);
        }
    } else {
        app->tab = (MenuTab)(CHANNEL_LIVE + which);
        load_tab(app, false);
    }
    app->menu_focus = MENU_GRID;
    app->dirty = true;
}

static void browse_direction(App *app, unsigned count, u64 key) {
    unsigned *selected = &app->selected[app->tab];
    bool home = app->tab == POPULAR, hero = home && !*selected;
    unsigned grid_index = home && !hero ? *selected - 1 : *selected;
    unsigned column = grid_index % PREVIEW_COLUMNS;
    bool top = home ? hero : *selected < PREVIEW_COLUMNS;
    unsigned next = *selected;

    if ((key & HidNpadButton_AnyLeft) && (!count || !column)) {
        app->menu_focus = MENU_RAIL;
        app->rail_choice = rail_index(app->root_tab);
    } else if ((key & HidNpadButton_AnyUp) && (!count || top)) {
        if (browse_selectors(app->tab)) {
            app->menu_focus = MENU_FILTERS;
            app->selector_choice = browse_selector(app);
        } else {
            app->menu_focus = MENU_TOOLBAR;
            app->tool_choice = TOOL_SEARCH;
        }
    } else if (count) {
        if (key & HidNpadButton_AnyLeft) --next;
        else if ((key & HidNpadButton_AnyRight) && !hero && column + 1 < PREVIEW_COLUMNS && next + 1 < count) ++next;
        else if (key & HidNpadButton_AnyUp) next = home && grid_index < PREVIEW_COLUMNS ? 0 : next - PREVIEW_COLUMNS;
        else if (key & HidNpadButton_AnyDown) {
            unsigned down = hero ? 1 : next + PREVIEW_COLUMNS;
            unsigned last_grid = count - 1 - (home && count > 1 ? 1u : 0u);
            if (down < count) next = down;
            else if (!hero && grid_index / PREVIEW_COLUMNS < last_grid / PREVIEW_COLUMNS) next = count - 1;
            else if (can_page(app, 1) || can_page(app, -1)) {
                app->menu_focus = MENU_PAGER;
                app->pager_choice = can_page(app, 1);
            }
        }
    } else if ((key & HidNpadButton_AnyDown) && (can_page(app, 1) || can_page(app, -1))) {
        app->menu_focus = MENU_PAGER;
        app->pager_choice = can_page(app, 1);
    }
    *selected = next;
    app->dirty = true;
}

static void browse_focus(App *app, u64 pressed) {
    if (app->menu_focus == MENU_RAIL) {
        if (pressed & (HidNpadButton_AnyUp | HidNpadButton_AnyDown))
            app->rail_choice = menu_rail_step(app->rail_choice, (pressed & HidNpadButton_AnyUp) != 0);
        else if (pressed & HidNpadButton_A) select_section(app, app->rail_choice);
        else if (pressed & HidNpadButton_AnyRight) app->menu_focus = MENU_GRID;
    } else if (app->menu_focus == MENU_FILTERS) {
        if ((pressed & HidNpadButton_AnyLeft) && app->selector_choice) --app->selector_choice;
        else if ((pressed & HidNpadButton_AnyRight) && app->selector_choice + 1 < browse_selectors(app->tab))
            ++app->selector_choice;
        else if (pressed & HidNpadButton_AnyDown) app->menu_focus = MENU_GRID;
        else if (pressed & HidNpadButton_AnyUp) {
            app->menu_focus = MENU_TOOLBAR;
            app->tool_choice = TOOL_SEARCH;
        } else if (pressed & HidNpadButton_A) choose_selector(app, app->selector_choice);
    } else if (app->menu_focus == MENU_PAGER) {
        if (pressed & HidNpadButton_AnyUp) app->menu_focus = MENU_GRID;
        else if ((pressed & HidNpadButton_AnyLeft) && can_page(app, -1)) app->pager_choice = 0;
        else if ((pressed & HidNpadButton_AnyRight) && can_page(app, 1)) app->pager_choice = 1;
        else if (pressed & HidNpadButton_A) {
            int step = app->pager_choice ? 1 : -1;
            if (can_page(app, step) && !app->page_step &&
                !(app->job.running && app->job.page_step && catalog_visible(app, &app->job))) {
                app->page_step = step;
                load_tab(app, true);
            }
        }
    } else {
        if ((pressed & HidNpadButton_AnyLeft) && app->tool_choice) --app->tool_choice;
        else if ((pressed & HidNpadButton_AnyRight) && app->tool_choice + 1 < MENU_TOOLS) ++app->tool_choice;
        else if (pressed & HidNpadButton_AnyDown) {
            app->menu_focus = browse_selectors(app->tab) ? MENU_FILTERS : MENU_GRID;
            if (app->menu_focus == MENU_FILTERS) app->selector_choice = browse_selector(app);
        } else if (pressed & HidNpadButton_A) {
            if (app->tool_choice == TOOL_SEARCH) {
                if (app->tab == CHANNEL_CLIPS) search_clips(app);
                else search_channels(app);
            }
            else if (app->tool_choice == TOOL_LINK) link_menu(app);
            else account_menu(app);
        }
    }
    app->dirty = true;
}

static bool menu_input(App *app, u64 pressed) {
    if (app->confirm_remember) {
        if (pressed & HidNpadButton_A) {
            app->remember_session = true;
            save_session(app, &app->account);
        }
        if (pressed & (HidNpadButton_A | HidNpadButton_B)) {
            app->confirm_remember = false;
            app->dirty = true;
        }
        return true;
    }
    if (app->playback_error) {
        if (pressed & HidNpadButton_A) {
            app->recoveries = 0;
            open_playback(app, app->failed_position);
        }
        else if (pressed & HidNpadButton_B) {
            app->playback_error = false;
            set_status(app, "");
        }
        return true;
    }
    if (app->sign_in_pending || (app->job.running && app->job.action == ACCOUNT_SIGN_IN)) {
        if (pressed & HidNpadButton_B) {
            if (app->sign_in_pending) app->sign_in_pending = false;
            else atomic_store(&app->job.cancelled, true);
            set_status(app, "Sign-in canceled.");
        }
        return true;
    }
    if (app->confirm_sign_out) {
        if (pressed & HidNpadButton_A) {
            app->ignore_job = app->job.running;
            if (app->ignore_job) atomic_store(&app->job.cancelled, true);
            account_clear(&app->account);
            clear_remote(app);
            bool removed = !app->remember_session || session_remove(session_path);
            app->remember_session = false;
            set_status(app, removed ? "Signed out of Twitch." : "Signed out. The SD card could not remove your saved session.");
        }
        if (pressed & (HidNpadButton_A | HidNpadButton_B)) {
            app->confirm_sign_out = false;
            app->dirty = true;
        }
        return true;
    }
    if (pressed & HidNpadButton_B) {
        if (app->menu_focus != MENU_GRID) {
            app->menu_focus = MENU_GRID;
            app->dirty = true;
            return true;
        }
        if (menu_root_tab(app->tab)) return false;
        app->tab = app->back_count ? app->back[--app->back_count] : app->root_tab;
        set_status(app, "");
        load_tab(app, false);
    } else if (pressed & HidNpadButton_X) account_menu(app);
    else if (pressed & HidNpadButton_ZR) link_menu(app);
    else if (pressed & HidNpadButton_Y) {
        if (app->tab == CHANNEL_CLIPS) search_clips(app);
        else search_channels(app);
    }
    else if (pressed & HidNpadButton_Plus) {
        unsigned selected = app->selected[app->tab];
        previews_refresh_live(&app->previews, active_list(app),
            menu_page(app->tab, selected), menu_page_size(app->tab, selected));
        load_tab(app, true);
    }
    else if (pressed & (HidNpadButton_L | HidNpadButton_R)) {
        bool previous = (pressed & HidNpadButton_L) != 0;
        unsigned count = browse_selectors(app->tab);
        if (count) {
            int next = (int)browse_selector(app) + (previous ? -1 : 1);
            if (next >= 0 && (unsigned)next < count) choose_selector(app, (unsigned)next);
        } else if (menu_root_tab(app->tab)) {
            select_section(app, menu_rail_step(rail_index(app->tab), previous));
        }
    } else if (app->menu_focus != MENU_GRID) browse_focus(app, pressed);
    else {
        const Channel *row = focused_channel(app);
        if (pressed & HidNpadButton_A) {
            if (row && row->kind == ITEM_CATEGORY) open_category(app, row);
            else if (row && row->kind == ITEM_CHANNEL) open_profile(app, row);
            else if (row) play_item(app, row);
            else if (!*app->account.access && app->tab == FOLLOWING) {
                if (*app->account.refresh) load_tab(app, true);
                else account_menu(app);
            }
        } else if (row && app->tab == CONTINUE_WATCHING && (pressed & HidNpadButton_Minus)) {
            if (library_remove_resume(&app->library, row->id)) {
                set_status(app, "Removed from Continue Watching.");
                save_library(app);
            }
        } else if (row && (pressed & HidNpadButton_ZL)) open_profile(app, row);
        else if (row && *row->login && (row->kind == ITEM_LIVE || row->kind == ITEM_CHANNEL) &&
                 (pressed & HidNpadButton_Minus)) {
            bool added = library_toggle_favorite(&app->library, row);
            set_status(app, added ? "Added to favorites." : "Removed from favorites.");
            save_library(app);
        } else if (pressed & (HidNpadButton_AnyUp | HidNpadButton_AnyDown | HidNpadButton_AnyLeft | HidNpadButton_AnyRight)) {
            browse_direction(app, active_list(app)->count, pressed);
        }
    }
    return true;
}

static unsigned seek_target(unsigned at, unsigned duration, int offset) {
    int64_t requested = (int64_t)at + offset;
    unsigned last = duration > 1000 ? duration - 1000 : 0;
    if (requested <= 0) return 0;
    return (uint64_t)requested < last ? (unsigned)requested : last;
}

static void set_paused(App *app, bool pause) {
    atomic_store(&app->playback->paused, pause);
    if (!pause) app->last_video_at = now_ns();
    if (pause && app->audio.started && R_SUCCEEDED(audoutStopAudioOut())) app->audio.started = false;
}

static bool player_action(App *app, PlayerAction action) {
    Playback *p = app->playback;
    switch (action) {
        case PLAYER_PLAY:
            set_paused(app, !atomic_load(&p->paused));
            break;
        case PLAYER_BACK:
        case PLAYER_FORWARD: {
            unsigned duration = atomic_load(&p->duration_ms);
            if (!duration) break;
            unsigned at = seek_target(atomic_load(&p->position_ms), duration, action == PLAYER_BACK ? -30000 : 30000);
            reopen_playback(app, at, "Seeking…");
            return true;
        }
        case PLAYER_SOUND:
            if (R_SUCCEEDED(audoutSetAudioOutVolume(app->player_muted ? 1 : 0)))
                app->player_muted = !app->player_muted;
            break;
        case PLAYER_SETTINGS:
            app->player_page = PLAYER_SETTINGS_MENU;
            app->player_setting = SETTINGS_QUALITY;
            break;
        case PLAYER_CHAT:
            app->chat_notice[0] = 0;
            app->chat_scroll = (ChatScroll){0};
            if (app->chat_visible && app->chat_view.state == CHAT_UNAVAILABLE) chat_request_stop(&app->chat);
            else app->chat_visible = !app->chat_visible;
            if (app->chat_visible) chat_start(&app->chat, p->channel, &app->account);
            app->chat_panel.valid = false;
            app->next_chat_poll = 0;
            break;
        case PLAYER_MESSAGE:
            if (app->chat_view.can_send) {
                /* The system keyboard runs synchronously on the UI thread. */
                static char draft[CHAT_TEXT_BYTES], message[CHAT_TEXT_BYTES];
                chat_draft(&app->chat, draft);
                message[0] = 0;
                app->chat_notice[0] = 0;
                if (keyboard("Twitch chat message", draft, message, sizeof(message)) && *message)
                    chat_queue(&app->chat, message);
            } else {
                const char *reason = app->chat_view.sending ? "Sending message…" :
                    app->chat_view.state == CHAT_CONNECTING ? "Chat is still connecting." :
                    app->chat_view.state == CHAT_UNAVAILABLE ? "Chat is unavailable." :
                    "Sign in again to allow chat messages.";
                snprintf(app->chat_notice, sizeof(app->chat_notice), "%s", reason);
            }
            app->chrome_until = now_ns() + UINT64_C(6000000000);
            break;
        case PLAYER_ACTIONS: break;
    }
    return false;
}

static int timeline_step(unsigned repeat, unsigned duration) {
    if (!repeat) return 5000;
    if (repeat < 5) return 10000;
    if (repeat < 14) return 30000;
    return duration / 100 > 60000 ? (int)(duration / 100) : 60000;
}

static bool player_submenu(App *app, u64 key, unsigned choices, unsigned repeat) {
    Playback *p = app->playback;
    switch (app->player_page) {
        case PLAYER_SETTINGS_MENU:
            if (key & HidNpadButton_B) app->player_page = PLAYER_CONTROLS;
            else if ((key & HidNpadButton_AnyUp) && app->player_setting > SETTINGS_QUALITY) --app->player_setting;
            else if ((key & HidNpadButton_AnyDown) && app->player_setting + 1 < PLAYER_SETTINGS_ROWS) ++app->player_setting;
            else if (key & HidNpadButton_A) {
                if (app->player_setting == SETTINGS_CAPTIONS) {
                    app->library.captions_on = !app->library.captions_on;
                    atomic_store(&p->want_captions, app->library.captions_on);
                    save_library(app);
                }
                else if (app->player_setting == SETTINGS_CAPTIONS_TEXT || app->player_setting == SETTINGS_CHAT_TEXT) {
                    TextSize *size = app->player_setting == SETTINGS_CAPTIONS_TEXT ?
                        &app->library.captions_size : &app->library.chat_size;
                    *size = (TextSize)((*size + 1) % TEXT_SIZE_COUNT);
                    save_library(app);
                }
                else if (choices) {
                    app->player_page = PLAYER_QUALITY_MENU;
                    app->player_choice = 0;
                    for (unsigned i = 0; i < choices; ++i)
                        if (p->qualities[i] == app->library.preferred_quality) app->player_choice = i + 1;
                }
            }
            break;
        case PLAYER_QUALITY_MENU:
            if (key & HidNpadButton_B) app->player_page = PLAYER_SETTINGS_MENU;
            else if ((key & HidNpadButton_AnyUp) && app->player_choice) --app->player_choice;
            else if ((key & HidNpadButton_AnyDown) && app->player_choice < choices) ++app->player_choice;
            else if (key & HidNpadButton_A) {
                unsigned choice = app->player_choice ? p->qualities[app->player_choice - 1] : 0;
                app->player_page = PLAYER_SETTINGS_MENU;
                if (choice != app->library.preferred_quality) {
                    app->library.preferred_quality = choice;
                    save_library(app);
                }
                app->fallback_quality = 0;
                app->recoveries = 0;
                unsigned effective = choice ? choice : p->recommended_quality;
                if (effective != p->playing_quality) {
                    reopen_playback(app, atomic_load(&p->position_ms), "Changing video quality…");
                    return true;
                }
            }
            break;
        case PLAYER_TIMELINE: {
            unsigned duration = atomic_load(&p->duration_ms);
            int step = timeline_step(repeat, duration);
            if (key & (HidNpadButton_B | HidNpadButton_AnyDown)) app->player_page = PLAYER_CONTROLS;
            else if (key & HidNpadButton_AnyLeft) app->seek_ms = seek_target(app->seek_ms, duration, -step);
            else if (key & HidNpadButton_AnyRight) app->seek_ms = seek_target(app->seek_ms, duration, step);
            else if (key & HidNpadButton_A) {
                reopen_playback(app, app->seek_ms, "Seeking…");
                return true;
            }
            break;
        }
        case PLAYER_CHAT_HISTORY:
            if (key & (HidNpadButton_B | HidNpadButton_AnyLeft)) app->player_page = PLAYER_CONTROLS;
            else if (key & HidNpadButton_A) app->chat_scroll = (ChatScroll){0};
            else if (key & (HidNpadButton_AnyUp | HidNpadButton_AnyDown))
                app->chat_scroll = chat_scroll_by_lines(&app->chat_view, app->chat_scroll,
                    app->library.chat_size, key & HidNpadButton_AnyUp ? -1 : 1, 3);
            break;
        case PLAYER_CONTROLS: break;
    }
    return false;
}

static bool player_controls(App *app, u64 *pressed, unsigned choices, unsigned repeat, uint64_t now) {
    Playback *p = app->playback;
    if (!app->watched) return false;
    u64 key = *pressed;
    if (app->player_page != PLAYER_CONTROLS) {
        *pressed = 0;
        if (key) app->chrome_until = now + UINT64_C(6000000000);
        return player_submenu(app, key, choices, repeat);
    }

    const u64 controls = HidNpadButton_A | HidNpadButton_AnyLeft | HidNpadButton_AnyRight |
                         HidNpadButton_AnyUp | HidNpadButton_AnyDown;
    if (!(key & controls) || (key & HidNpadButton_B)) return false;
    bool visible = now < app->chrome_until || atomic_load(&p->paused);
    app->chrome_until = now + UINT64_C(6000000000);
    if (!visible) return false;

    if ((key & HidNpadButton_AnyUp) && p->kind == ITEM_VIDEO) {
        unsigned duration = atomic_load(&p->duration_ms);
        if (duration) {
            app->seek_ms = seek_target(atomic_load(&p->position_ms), duration, 0);
            app->player_page = PLAYER_TIMELINE;
        }
        return false;
    }

    PlayerAction actions[PLAYER_ACTIONS];
    unsigned count = player_actions(p->kind, app->chat_visible, actions);
    if (app->player_focus >= count) app->player_focus = count - 1;
    if ((key & HidNpadButton_AnyLeft) && app->player_focus) --app->player_focus;
    else if (key & HidNpadButton_AnyRight) {
        if (app->player_focus + 1 < count) ++app->player_focus;
        else if (app->chat_visible) app->player_page = PLAYER_CHAT_HISTORY;
    }
    else if (key & HidNpadButton_A) return player_action(app, actions[app->player_focus]);
    return false;
}

static bool update_chat_panel(App *app, const PlayerOverlay *view) {
    ChatPanel *panel = &app->chat_panel;
    bool history = view->page == PLAYER_CHAT_HISTORY;
    if (panel->valid && panel->revision == app->chat_revision && panel->touch_input == view->touch_input &&
        panel->scroll.end == view->chat_scroll.end && panel->scroll.hidden_lines == view->chat_scroll.hidden_lines &&
        panel->size == view->chat_size && panel->history == history && !strcmp(panel->notice, app->chat_notice))
        return false;
    render_chat(chat_pixels, &app->chat_view, app->playback->channel, app->chat_notice, view);
    *panel = (ChatPanel){.valid = true, .history = history, .revision = app->chat_revision,
        .touch_input = view->touch_input,
        .scroll = view->chat_scroll, .size = view->chat_size};
    strcpy(panel->notice, app->chat_notice);
    return true;
}

static void playback_frame(PlayerDisplay *display, App *app, u64 pressed, unsigned repeat, bool touch_redraw) {
    Playback *p = app->playback;
    unsigned choices = atomic_load(&p->quality_count);
    uint64_t now = now_ns();
#ifdef IZTWINS2_IZA
    if (p->last_tick && app->watched && !atomic_load(&p->paused) && !cancelled(p)) {
        uint64_t gap = now - p->last_tick;
        if (gap > 33333334) ++p->ticks33;
        if (gap > p->max_tick) p->max_tick = gap;
        p->chat_ticks += app->chat_visible;
    }
    p->last_tick = now;
#endif
    bool redraw = pressed != 0 || touch_redraw;
    if (!cancelled(p)) player_controls(app, &pressed, choices, repeat, now);
    if (pressed & HidNpadButton_B) {
        app->playback_closing = true;
        app->restart_pending = false;
        atomic_store(&p->stop, true);
        atomic_store(&p->subtitle_stop, true);
    }
    unsigned video_index = atomic_load(&p->video_in);
    if (video_index != app->last_video_index) {
        app->last_video_index = video_index;
        app->last_video_at = now;
    }
    if (!cancelled(p) && !app->watched && video_index) {
        app->watched = true;
        app->playback_since = now;
        app->chrome_until = now + UINT64_C(6000000000);
        if (app->pause_when_ready) set_paused(app, true);
        if (p->kind == ITEM_LIVE) {
            library_add_recent(&app->library, app->last_channel);
            save_library(app);
        }
    }
    bool paused = atomic_load(&p->paused);
    bool network_done = atomic_load(&p->network_done);
    bool network_error = network_done && *p->network_error;
    if (paused && network_error) {
        atomic_store(&p->stop, true);
        atomic_store(&p->subtitle_stop, true);
    }
    if (app->playback_since && now - app->playback_since >= UINT64_C(60000000000)) app->recoveries = 0;
    if (app->watched && !paused && !cancelled(p) && p->kind == ITEM_VIDEO && now >= app->next_position_save) {
        save_position(app, false);
        app->next_position_save = now + UINT64_C(30000000000);
    }
    if (!cancelled(p) && !paused && !audio_pump(&app->audio, p)) {
        set_status(app, "Could not play audio.");
        app->playback_error = true;
        atomic_store(&p->stop, true);
    }
    if (app->chat_visible && now >= app->next_chat_poll) {
        chat_snapshot(&app->chat, &app->chat_view, &app->chat_revision);
        app->next_chat_poll = now + UINT64_C(100000000);
    }
    bool chrome = now < app->chrome_until;
    redraw |= chrome != app->player_chrome;
    app->player_chrome = chrome;
    bool buffering = app->watched && !paused && !network_done && app->last_video_at &&
        now - app->last_video_at >= UINT64_C(850000000);
#ifdef IZTWINS2_IZA
    if (buffering && !p->waiting_video) ++p->stalls;
    p->waiting_video = buffering;
#endif
    const char *busy = app->playback_closing ? "Closing player…" :
        app->restart_pending || !app->watched ? app->buffering_text :
        paused && network_error ? "Connection lost…" :
        buffering ? (atomic_load(&p->reconnecting) ? "Reconnecting to Twitch…" : "Buffering video…") : NULL;
    unsigned phase = (unsigned)(now / UINT64_C(110000000) % 8);
    redraw |= busy && phase != app->busy_phase;
    redraw |= busy != app->shown_busy;
    app->busy_phase = phase;
    app->shown_busy = busy;
    if (!cancelled(p) || app->playback_closing || app->restart_pending || (paused && network_error)) {
        char caption[CAPTION_BYTES] = "";
        char caption_track[sizeof(p->subtitle.name)] = "";
        PlayerOverlay overlay = {
            .title = app->playing_title, .channel = *app->playing.name ? app->playing.name : p->channel,
            .caption = caption, .caption_track = caption_track, .buffering = busy,
            .touch = &app->player_touch, .chat_touch = &app->chat_touch, .touch_input = app->touch_input,
            .qualities = p->qualities, .rates = p->quality_fps,
            .count = choices, .choice = app->player_choice, .preferred = app->library.preferred_quality,
            .focus = app->player_focus, .setting = app->player_setting,
            .quality = choices ? p->playing_quality : 0, .fps = choices ? p->playing_fps : 0,
            .position_ms = atomic_load(&p->position_ms), .duration_ms = atomic_load(&p->duration_ms),
            .seek_ms = app->seek_ms, .busy_phase = phase, .kind = p->kind, .page = app->player_page, .chrome = chrome,
            .paused = paused, .captions_on = app->library.captions_on, .muted = app->player_muted, .chat = app->chat_visible,
            .stalled = buffering && !app->restart_pending && !app->playback_closing,
            .chat_failed = app->chat_visible && app->chat_view.state == CHAT_UNAVAILABLE,
            .chat_can_send = app->chat_visible && app->chat_view.can_send,
            .captions_size = app->library.captions_size, .chat_size = app->library.chat_size,
            .chat_scroll = app->chat_scroll,
        };
        if (app->library.captions_on) {
            mutexLock(&p->caption_lock);
            if (p->subtitle_ready) {
                uint64_t pts = atomic_load(&p->video_pts);
                uint64_t position = p->kind == ITEM_LIVE ? pts : (uint64_t)overlay.position_ms * 1000;
                strcpy(caption_track, p->subtitle.name);
                overlay.caption_seen = true;
                for (unsigned i = 0; i < p->subtitle_count; ++i) {
                    unsigned index = (p->subtitle_next + WEBVTT_CACHED_CUES - 1 - i) % WEBVTT_CACHED_CUES;
                    const WebVttCue *cue = &p->subtitle_cues[index];
                    if (!webvtt_active(cue, pts, position)) continue;
                    strcpy(caption, cue->text);
                    break;
                }
            } else {
                memcpy(caption, p->caption, sizeof(caption));
                overlay.caption_seen = p->caption_seen;
            }
            mutexUnlock(&p->caption_lock);
        }
#ifdef IZTWINS2_IZA
        uint64_t render_start = now_ns();
#endif
        const bool chat_dirty = app->chat_visible && update_chat_panel(app, &overlay);
        if (!draw_video(display, p, &overlay, redraw, chat_dirty)) {
            set_status(app, "Not enough memory to display this video.");
            app->playback_error = true;
            atomic_store(&p->stop, true);
        }
#ifdef IZTWINS2_IZA
        uint64_t render_time = now_ns() - render_start;
        if (render_time > p->max_draw) p->max_draw = render_time;
#endif
    }
    if (atomic_load(&p->decode_done) && network_done) {
        atomic_store(&p->subtitle_stop, true);
        if (!atomic_load(&p->subtitle_done)) return;
        if (app->restart_pending) {
            finish_restart(app);
            return;
        }
        if (!cancelled(p)) {
            u32 pending = 0;
            if (paused || (R_SUCCEEDED(audoutGetAudioOutBufferCount(&pending)) && pending)) return;
        }
        if (network_error && !*p->decode_error && !app->playback_error && !app->playback_closing && !paused &&
            p->kind == ITEM_LIVE && (app->watched || p->has_frame) && app->recoveries < 2) {
            ++app->recoveries;
            if (!app->library.preferred_quality && choices) app->fallback_quality = lower_quality(p, choices);
            reopen_playback(app, 0, "Reconnecting to Twitch…");
            finish_restart(app);
            return;
        }
        save_position(app, !cancelled(p) && !*p->decode_error && !*p->network_error);
        restore_played_card(app);
        if (app->playback_closing) {
            app->playback_error = false;
            set_status(app, "");
        } else if (*p->decode_error || *p->network_error) {
            set_status(app, *p->decode_error ? p->decode_error : p->network_error);
            app->playback_error = true;
        }
        else if (!*app->status) set_status(app, cancelled(p) ? "" : "Playback ended.");
        if (app->playback_error) {
            app->failed_position = atomic_load(&p->position_ms);
            app->chat_visible = false;
            app->player_focus = 0;
        }
        chat_request_stop(&app->chat);
        clear(p);
        app->playback = NULL;
        audio_close(&app->audio);
        app->dirty = true;
    }
}

#ifndef IZTWINS2_IZA
static bool launcher_program(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    struct stat stat;
    unsigned char data[4096];
    size_t bytes = !fstat(fileno(file), &stat) && S_ISREG(stat.st_mode) && stat.st_size >= (off_t)sizeof(data) ?
        fread(data, 1, sizeof(data), file) : 0;
    fclose(file);
    if (bytes != sizeof(data)) return false;
    NroHeader header;
    memcpy(&header, data + sizeof(NroStart), sizeof(header));
    NroOunceInfo info;
    return header.magic == NROHEADER_MAGIC && header.size >= sizeof(data) && header.size <= (uint64_t)stat.st_size &&
        nroGetOunceInfo(data, bytes, &info) && info.platform == NroPlatform_Switch2;
}

static bool launched_from_iz2s(int argc, char **argv) {
    static const char modules[] = "sdmc:/iz2s/modules/";
    return envHasNextLoad() && argc > 0 && argv && argv[0] &&
        !strncmp(argv[0], modules, sizeof(modules) - 1);
}

static bool installed_iz2s_runtime(void) {
    struct stat menu, runtime;
    return !stat("sdmc:/iz2s/izlauncher.iza", &menu) && S_ISREG(menu.st_mode) &&
           !stat("sdmc:/iz2s/core/bin/game/main", &runtime) && S_ISREG(runtime.st_mode);
}

static const char *launcher_return_path(void) {
    if (launcher_program(launcher_path)) return launcher_path;
    return launcher_program(launcher_companion) ? launcher_companion : NULL;
}

static bool extract_launcher(void) {
    static const char resource[] = "iztwi:/return-iz2s.nro";
    static const char staging[] = "sdmc:/iz2s/modules/iztwins2/.return-iz2s.nro.tmp";
    struct stat info;
    if (!stat(launcher_companion, &info) || errno != ENOENT) return false;
    if (R_FAILED(romfsMountSelf("iztwi"))) return false;

    FILE *source = fopen(resource, "rb");
    FILE *dest = source ? fopen(staging, "wb") : NULL;
    bool staged = dest != NULL, ok = staged;
    if (dest) {
        unsigned char buffer[65536];
        size_t count;
        while ((count = fread(buffer, 1, sizeof(buffer), source))) {
            if (fwrite(buffer, 1, count, dest) == count) continue;
            ok = false;
            break;
        }
        if (ferror(source)) ok = false;
        if (fclose(dest)) ok = false;
    }
    if (source) fclose(source);
    romfsUnmount("iztwi");
    if (ok && launcher_program(staging) && stat(launcher_companion, &info) && errno == ENOENT &&
        !rename(staging, launcher_companion)) return true;
    if (staged) remove(staging);
    return false;
}

#endif

static void request_shutdown(App *app) {
    if (app->job.running) atomic_store(&app->job.cancelled, true);
    if (app->previews.running) atomic_store(&app->previews.cancelled, true);
    chat_request_stop(&app->chat);
    if (app->playback) {
        atomic_store(&app->playback->stop, true);
        atomic_store(&app->playback->subtitle_stop, true);
    }
}

enum {
    WAIT_ACCOUNT = 1, WAIT_PREVIEWS = 2, WAIT_CHAT = 4, WAIT_NETWORK = 8,
    WAIT_DECODER = 16, WAIT_SUBTITLES = 32,
};

static unsigned shutdown_pending(const App *app) {
    const Playback *p = app->playback;
    return (app->job.running && !atomic_load(&app->job.finished) ? WAIT_ACCOUNT : 0) |
        (app->previews.running && !atomic_load(&app->previews.finished) ? WAIT_PREVIEWS : 0) |
        (app->chat.running && !atomic_load(&app->chat.finished) ? WAIT_CHAT : 0) |
        (p && !atomic_load(&p->network_done) ? WAIT_NETWORK : 0) |
        (p && !atomic_load(&p->decode_done) ? WAIT_DECODER : 0) |
        (p && !atomic_load(&p->subtitle_done) ? WAIT_SUBTITLES : 0);
}

static void finish_player(App *app) {
    chat_stop(&app->chat);
    save_position(app, false);
    clear(app->playback);
    app->playback = NULL;
#ifdef IZTWINS2_IZA
    if (app->library_pending) save_library(app);
#endif
    account_join(&app->job);
    if (app->remember_session && !app->ignore_job && *app->job.account.refresh &&
        (app->job.action == ACCOUNT_CHECK || app->job.action == ACCOUNT_FOLLOWING) &&
        strcmp(app->account.refresh, app->job.account.refresh)) save_session(app, &app->job.account);
    account_clear(&app->job.account);
    account_clear(&app->account);
    previews_close(&app->previews);
    audio_close(&app->audio);
    typography_close();
}

typedef struct {
    TouchGesture gesture;
    TouchTarget target;
    MenuTab tab;
    PlayerPage previous_page;
    unsigned generation, previous_seek;
    int scrolled;
    bool playback, scrubbing;
    ItemKind card_kind;
    char card_id[sizeof(((Channel *)0)->id)], card_login[CHANNEL_LOGIN_BYTES];
} TouchCapture;

typedef struct {
    uint64_t previous, repeat_at;
    unsigned repeats;
    TouchCapture touch;
} PlayerInput;

static u64 touch_key(unsigned key) {
    static const u64 buttons[KEY_COUNT] = {
        [KEY_SELECT] = HidNpadButton_A, [KEY_BACK] = HidNpadButton_B,
        [KEY_ACCOUNT] = HidNpadButton_X, [KEY_SEARCH] = HidNpadButton_Y,
        [KEY_REFRESH] = HidNpadButton_Plus, [KEY_CHANNEL] = HidNpadButton_ZL,
        [KEY_SAVE] = HidNpadButton_Minus, [KEY_UP] = HidNpadButton_Up, [KEY_DOWN] = HidNpadButton_Down,
    };
    return key < KEY_COUNT ? buttons[key] : 0;
}

static TouchTarget touch_target(App *app, int x, int y) {
    if (!app->playback) return touch_layout_find(&app->menu_touch, x, y);
    if (app->chat_visible && x >= PLAYER_CHAT_EDGE) {
        TouchTarget target = touch_layout_find(&app->chat_touch, x - PLAYER_CHAT_EDGE, y);
        return target.kind == HIT_NONE ? (TouchTarget){HIT_PANEL, 0} : target;
    }
    return touch_layout_find(&app->player_touch, x, y);
}

static void touch_page(App *app, unsigned direction) {
    app->menu_focus = MENU_PAGER;
    app->pager_choice = direction;
    browse_focus(app, HidNpadButton_A);
}

static void touch_browse_swipe(App *app, int step) {
    unsigned *selected = &app->selected[app->tab];
    unsigned first = menu_page(app->tab, *selected);
    unsigned next = first + menu_page_size(app->tab, *selected);
    app->menu_focus = MENU_GRID;
    if (step < 0 && first) *selected = first - 1;
    else if (step > 0 && next < active_list(app)->count) *selected = next;
    else if (can_page(app, step)) touch_page(app, step > 0);
    app->dirty = true;
}

static bool touch_browse(App *app, const TouchCapture *capture, const TouchEvent *event, TouchTarget target) {
    TouchTarget start = capture->target;
    if (event->dragged) {
        if (start.kind == HIT_MENU_LIST || start.kind == HIT_MENU_CARD) {
            /* A dialog can become visible while a request completes. */
            if (!app->confirm_remember && !app->confirm_sign_out && !app->playback_error &&
                !app->sign_in_pending && !(app->job.running && app->job.action == ACCOUNT_SIGN_IN)) {
                int delta = abs(event->dx) > abs(event->dy) ? event->dx : event->dy;
                if (abs(delta) >= 55) touch_browse_swipe(app, delta < 0 ? 1 : -1);
            }
        }
        return true;
    }
    if (start.kind != target.kind || start.index != target.index) return true;
    switch (target.kind) {
        case HIT_MENU_SECTION:
            select_section(app, target.index);
            break;
        case HIT_MENU_TOOL:
            app->menu_focus = MENU_TOOLBAR;
            app->tool_choice = target.index;
            browse_focus(app, HidNpadButton_A);
            if (app->menu_focus == MENU_TOOLBAR) app->menu_focus = MENU_GRID;
            break;
        case HIT_MENU_FILTER:
            if (target.index < browse_selectors(app->tab)) choose_selector(app, target.index);
            break;
        case HIT_MENU_CARD: {
            const ChannelList *list = active_list(app);
            if (target.index >= list->count) break;
            const Channel *item = &list->items[target.index];
            if (item->kind != capture->card_kind || strcmp(item->id, capture->card_id) ||
                strcmp(item->login, capture->card_login)) break;
            app->menu_focus = MENU_GRID;
            app->selected[app->tab] = target.index;
            app->dirty = true;
            if (!event->held) return menu_input(app, HidNpadButton_A);
            break;
        }
        case HIT_MENU_PAGE:
            touch_page(app, target.index);
            break;
        case HIT_KEY:
            return menu_input(app, touch_key(target.index));
        default:
            break;
    }
    return true;
}

static void touch_scrub(App *app, int x) {
    int pane = app->chat_visible ? PLAYER_CHAT_EDGE : DISPLAY_WIDTH;
    int bar = pane - 2 * PLAYER_CONTROL_INSET;
    int offset = x - PLAYER_CONTROL_INSET;
    if (offset < 0) offset = 0;
    if (offset > bar) offset = bar;
    unsigned duration = atomic_load(&app->playback->duration_ms);
    app->seek_ms = seek_target((unsigned)((uint64_t)offset * duration / bar), duration, 0);
}

static bool touch_chat_drag(App *app, TouchCapture *capture, const TouchEvent *event) {
    if (!app->chat_visible || (!capture->scrolled && abs(event->dx) > abs(event->dy))) return false;
    int gap = chat_line_height(app->library.chat_size);
    int lines = (event->dy - capture->scrolled) / gap;
    if (!lines) return false;
    app->player_page = PLAYER_CHAT_HISTORY;
    app->chat_scroll = chat_scroll_by_lines(&app->chat_view, app->chat_scroll, app->library.chat_size,
                                           lines > 0 ? -1 : 1, (unsigned)abs(lines));
    capture->scrolled += lines * gap;
    return true;
}

static bool touch_playback(App *app, const TouchEvent *event, TouchTarget target, u64 *pressed, uint64_t now) {
    Playback *p = app->playback;
    if (target.kind == HIT_KEY) {
        *pressed |= touch_key(target.index);
        return true;
    }
    if (cancelled(p) || !app->watched) return false;
    unsigned choices = atomic_load(&p->quality_count);
    bool visible = now < app->chrome_until || atomic_load(&p->paused);
    app->chrome_until = now + UINT64_C(6000000000);
    switch (target.kind) {
        case HIT_NONE:
            if (app->player_page != PLAYER_CONTROLS) player_submenu(app, HidNpadButton_B, choices, 0);
            else if (visible) app->chrome_until = 0;
            break;
        case HIT_PLAYER_ACTION: {
            PlayerAction actions[PLAYER_ACTIONS];
            unsigned count = player_actions(p->kind, app->chat_visible, actions);
            if (target.index >= count) return false;
            app->player_page = PLAYER_CONTROLS;
            app->player_focus = target.index;
            player_action(app, actions[target.index]);
            break;
        }
        case HIT_SETTING:
            if (app->player_page != PLAYER_SETTINGS_MENU || target.index >= PLAYER_SETTINGS_ROWS) return false;
            app->player_setting = (PlayerSetting)target.index;
            player_submenu(app, HidNpadButton_A, choices, 0);
            break;
        case HIT_QUALITY:
            if (app->player_page != PLAYER_QUALITY_MENU || target.index > choices) return false;
            app->player_choice = target.index;
            player_submenu(app, HidNpadButton_A, choices, 0);
            break;
        case HIT_CHAT_BODY:
            if (!app->chat_visible || event->dragged) return false;
            app->player_page = PLAYER_CHAT_HISTORY;
            break;
        case HIT_CHAT_BACK:
            app->player_page = PLAYER_CONTROLS;
            break;
        case HIT_CHAT_LIVE:
            app->chat_scroll = (ChatScroll){0};
            break;
        case HIT_CHAT_MESSAGE:
            player_action(app, PLAYER_MESSAGE);
            break;
        default:
            return false;
    }
    return true;
}

static bool touch_frame(App *app, TouchCapture *capture, TouchContact contact, uint64_t now,
                        u64 *pressed, bool *redraw) {
    bool same_surface = capture->playback == (app->playback != NULL) &&
        capture->generation == app->playback_generation &&
        (capture->playback || capture->tab == app->tab);
    if (*pressed || (capture->gesture.tracking && !same_surface)) {
        if (capture->scrubbing && same_surface) {
            app->player_page = capture->previous_page;
            app->seek_ms = capture->previous_seek;
            *redraw = true;
        }
        capture->scrubbing = false;
        capture->gesture = (TouchGesture){.blocked = contact.state != CONTACT_IDLE};
        if (*pressed && app->touch_input) {
            app->touch_input = false;
            app->dirty = *redraw = true;
        }
        return true;
    }
    TouchEvent event = touch_update(&capture->gesture, contact, now);
    if (event.phase == TOUCH_NO_EVENT) return true;
    TouchTarget target = touch_target(app, event.x, event.y);
    if (event.phase == TOUCH_BEGIN) {
        capture->target = target;
        capture->tab = app->tab;
        capture->playback = app->playback != NULL;
        capture->generation = app->playback_generation;
        capture->scrolled = 0;
        if (target.kind == HIT_MENU_CARD && target.index < active_list(app)->count) {
            const Channel *item = &active_list(app)->items[target.index];
            capture->card_kind = item->kind;
            strcpy(capture->card_id, item->id);
            strcpy(capture->card_login, item->login);
        }
        capture->scrubbing = target.kind == HIT_TIMELINE && app->playback &&
            app->watched && !cancelled(app->playback) && atomic_load(&app->playback->duration_ms);
        if (capture->scrubbing) {
            capture->previous_page = app->player_page;
            capture->previous_seek = app->seek_ms;
            app->player_page = PLAYER_TIMELINE;
            touch_scrub(app, event.x);
            *redraw = true;
        }
        return true;
    }
    if (event.phase == TOUCH_CANCEL) {
        if (capture->scrubbing) {
            app->player_page = capture->previous_page;
            app->seek_ms = capture->previous_seek;
            *redraw = true;
        }
        capture->scrubbing = false;
        return true;
    }
    if (event.phase == TOUCH_RELEASE && !app->touch_input) {
        app->touch_input = true;
        app->dirty = *redraw = true;
    }
    if (capture->scrubbing) {
        touch_scrub(app, event.x);
        *redraw = true;
        if (event.phase == TOUCH_RELEASE) {
            capture->scrubbing = false;
            if (!cancelled(app->playback))
                player_submenu(app, HidNpadButton_A, atomic_load(&app->playback->quality_count), 0);
        }
    } else if (app->playback && event.dragged && capture->target.kind == HIT_CHAT_BODY) {
        if (touch_chat_drag(app, capture, &event)) *redraw = true;
    } else if (event.phase == TOUCH_RELEASE) {
        if (!app->playback) return touch_browse(app, capture, &event, target);
        if (!event.dragged && capture->target.kind == target.kind && capture->target.index == target.index)
            if (touch_playback(app, &event, target, pressed, now)) *redraw = true;
    }
    return true;
}

static bool player_frame(PlayerDisplay *display, App *app, PlayerInput *input, uint64_t held, TouchContact contact) {
    const uint64_t directions = HidNpadButton_AnyUp | HidNpadButton_AnyDown |
                                HidNpadButton_AnyLeft | HidNpadButton_AnyRight;
    uint64_t pressed = held & ~input->previous;
    input->previous = held;
    const uint64_t now = now_ns();
    unsigned direction_repeat = 0;
    if (pressed & directions) {
        input->repeat_at = now + 350000000;
        input->repeats = 0;
    } else if (!(held & directions)) {
        input->repeat_at = 0;
        input->repeats = 0;
    } else if (input->repeat_at && now >= input->repeat_at) {
        pressed |= held & directions;
        input->repeat_at = now + 105000000;
        direction_repeat = ++input->repeats;
    }
    bool touch_redraw = false;
    if (!touch_frame(app, &input->touch, contact, now, &pressed, &touch_redraw)) return false;
    if (app->playback) playback_frame(display, app, pressed, direction_repeat, touch_redraw);
    else if (pressed && !menu_input(app, pressed)) return false;
    poll_job(app);
    if (chat_reap(&app->chat) && app->playback && app->chat_visible && !app->playback_closing)
        chat_start(&app->chat, app->playback->channel, &app->account);
    load_pending(app, now);
    if (previews_collect(&app->previews, now)) app->dirty = true;
    if (!app->playback && !app->confirm_sign_out && !app->sign_in_pending &&
        !(app->job.running && app->job.action == ACCOUNT_SIGN_IN))
        previews_request(&app->previews, active_list(app),
            menu_page(app->tab, app->selected[app->tab]), menu_page_size(app->tab, app->selected[app->tab]), now);
    if (!app->job.running && now >= app->retry_check_at && account_due(&app->account) &&
        !app->confirm_sign_out && !app->confirm_remember)
        if (!begin_job(app, ACCOUNT_CHECK, NULL)) app->retry_check_at = now + UINT64_C(30000000000);
#ifdef IZTWINS2_IZA
    if (app->library_pending && !app->playback) save_library(app);
#endif
    if (app->dirty && !app->playback) draw_menu(display, app);
    return true;
}

#ifndef IZTWINS2_IZA
static void exit_note(FILE *log, uint64_t started, const char *message) {
    if (!log) return;
    fprintf(log, "[%llu ms] %s\n", (unsigned long long)((now_ns() - started) / 1000000), message);
    fflush(log);
}

static void finish_exit(App *app, Framebuffer *display, bool requested, int argc, char **argv) {
    const uint64_t started = now_ns();
    request_shutdown(app);
    FILE *log = app->have_sd ? fopen(exit_log_path, "w") : NULL;
    exit_note(log, started, requested ? "Exit requested in IZtwiNS2 (0.11.0)." :
                                       "Exit requested by the system (0.11.0).");
    const char *target = NULL;
    bool iz2s_launch = requested && app->have_sd && launched_from_iz2s(argc, argv);
    /* The integrated IZ2S loader already returns to the installed menu.
     * Reserve next-load overrides for older standalone installations. */
    bool standard_return = iz2s_launch && installed_iz2s_runtime();
    if (iz2s_launch && !standard_return) {
        target = launcher_return_path();
        if (!target) {
            app->returning_to_launcher = true;
            app->exit_detail = "Preparing the IZ2S launcher…";
            draw_menu(display, app);
            if (extract_launcher()) target = launcher_companion;
            exit_note(log, started, target ? "Prepared the bundled IZ2S launcher." :
                                             "No launcher return entry was available.");
        }
    }
    app->returning_to_launcher = standard_return || target != NULL;
    bool visible = requested;
    unsigned last = UINT_MAX;
    for (;;) {
        unsigned pending = shutdown_pending(app);
        if (pending != last) {
            if (log) {
                fprintf(log, "[%llu ms] Workers active:%s%s%s%s%s%s%s\n",
                    (unsigned long long)((now_ns() - started) / 1000000), pending ? "" : " none",
                    pending & WAIT_ACCOUNT ? " account" : "", pending & WAIT_PREVIEWS ? " previews" : "",
                    pending & WAIT_CHAT ? " chat" : "", pending & WAIT_NETWORK ? " video network" : "",
                    pending & WAIT_DECODER ? " video decoder" : "", pending & WAIT_SUBTITLES ? " subtitles" : "");
                fflush(log);
            }
            if (visible) {
                app->exit_detail = pending ? "Closing Twitch connections and playback…" : "Closing player…";
                draw_menu(display, app);
            }
            last = pending;
        }
        if (!pending) break;
        if (visible) visible = appletMainLoop();
        svcSleepThread(10000000);
    }

    finish_player(app);

    exit_note(log, started, "Closing framebuffer.");
    framebufferClose(display);
    exit_note(log, started, "Framebuffer closed.");
    if (target) {
        char arguments[sizeof(launcher_companion) + 2];
        snprintf(arguments, sizeof(arguments), "\"%s\"", target);
        Result rc = envSetNextLoad(target, arguments);
        if (log) {
            fprintf(log, "IZ2S next load: 0x%08x, %s.\n", (unsigned)rc, target);
            fflush(log);
        }
    } else exit_note(log, started, standard_return ? "Using the installed IZ2S runtime return." :
                                                    "Using the loader's default exit.");
    exit_note(log, started, "Leaving IZtwiNS2 main.");
    if (log) fclose(log);
    if (app->own_sd) fsdevUnmountDevice("sdmc");
}

int main(int argc, char **argv) {
    Framebuffer display = {0};
    if (R_FAILED(framebufferCreate(&display, nwindowGetDefault(), DISPLAY_WIDTH, DISPLAY_HEIGHT, PIXEL_FORMAT_RGBA_8888, 2))) return 1;
    if (R_FAILED(framebufferMakeLinear(&display))) { framebufferClose(&display); return 1; }
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    hidInitializeTouchScreen();
    PadState pad;
    padInitializeDefault(&pad);
    static App app = {.dirty = true, .tab = POPULAR, .root_tab = POPULAR};
    if (fsdevGetDeviceFileSystem("sdmc")) app.have_sd = true;
    else app.have_sd = app.own_sd = R_SUCCEEDED(fsdevMountSdmc());
    if (app.have_sd) {
        mkdir("sdmc:/switch", 0777);
        mkdir("sdmc:/switch/IZtwiNS2", 0777);
        if (!library_load(&app.library, library_path) && errno == ENOENT &&
            library_load(&app.library, "sdmc:/switch/twiNX/library.txt"))
            save_library(&app);
        app.remember_session = session_load(&app.account, session_path);
    }
    load_tab(&app, false);

    bool requested_exit = false;
    PlayerInput input = {.touch.gesture.blocked = true};
    while (appletMainLoop()) {
        padUpdate(&pad);
        HidTouchScreenState state = {0};
        hidGetTouchScreenStates(&state, 1);
        const HidTouchState *finger = &state.touches[0];
        TouchContact contact = touch_contact(state.count, finger->attributes, finger->finger_id, finger->x, finger->y);
        if (!player_frame(&display, &app, &input, padGetButtons(&pad), contact)) {
            requested_exit = true;
            break;
        }
        svcSleepThread(6000000);
    }
    finish_exit(&app, &display, requested_exit, argc, argv);
    return 0;
}
#else
static App native_app = {.dirty = true, .tab = POPULAR, .root_tab = POPULAR};
static PlayerInput native_input = {.touch.gesture.blocked = true};

void iztwi_player_start(bool have_storage, uint64_t held) {
    native_app.have_sd = have_storage;
    native_input.previous = held;
    if (have_storage) {
        library_load(&native_app.library, library_path);
        native_app.remember_session = session_load(&native_app.account, session_path);
    }
    load_tab(&native_app, false);
}

bool iztwi_player_frame(PlayerDisplay *display, uint64_t held, TouchContact contact) {
    return player_frame(display, &native_app, &native_input, held, contact);
}

void iztwi_player_request_stop(void) {
    request_shutdown(&native_app);
}

bool iztwi_player_workers_stopped(void) {
    return !shutdown_pending(&native_app);
}

void iztwi_player_stop(void) {
    finish_player(&native_app);
}
#endif
