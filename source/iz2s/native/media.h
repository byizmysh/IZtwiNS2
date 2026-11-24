#ifndef IZTWINS2_MEDIA_H
#define IZTWINS2_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "twitch.h"

enum { AUDIO_RATE = 48000, CAPTION_BYTES = 1024 };

typedef struct AVCodecContext CaptionDecoder;

typedef enum {
    MEDIA_RGBA, MEDIA_601_LIMITED, MEDIA_601_FULL, MEDIA_709_LIMITED, MEDIA_709_FULL
} MediaPixelFormat;

typedef struct {
    const uint8_t *plane[3];
    int stride[3];
    unsigned width, height;
    MediaPixelFormat format;
} MediaYuvFrame;

typedef struct {
    void *user;
    bool (*cancelled)(void *);
    bool (*video)(void *, const uint8_t *, int64_t pts_us, unsigned width, unsigned height);
    bool (*audio)(void *, const int16_t *, unsigned frames);
    CaptionDecoder *captions;
    void (*caption)(void *, const char *);
    void (*clip_duration)(void *, unsigned ms);
    unsigned skip_ms;
    bool (*video_yuv)(void *, const MediaYuvFrame *, int64_t pts_us);
} MediaOutput;

CaptionDecoder *media_captions_open(void);
void media_captions_close(CaptionDecoder **);
void media_caption_packet(const MediaOutput *, const uint8_t *, size_t, int64_t pts_us);

/* Demuxes a complete MPEG-TS segment supplied in memory. */
int decode_segment(const uint8_t *, size_t, const MediaOutput *);
int decode_fragment(const uint8_t *, size_t, const MediaOutput *);
int decode_clip(const uint8_t *, size_t, const MediaOutput *);

#endif
