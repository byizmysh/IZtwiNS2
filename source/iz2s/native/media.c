/* SPDX-License-Identifier: GPL-3.0-only */
#include "media.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const uint8_t *input;
    size_t size, offset;
    const MediaOutput *out;
} Input;

typedef struct {
    AVCodecContext *video, *audio;
    SwrContext *resampler;
    struct SwsContext *scaler;
    uint8_t *rgba;
    size_t rgba_capacity;
    int16_t *pcm;
    int64_t trim_before;
    bool trim_started;
} Decoder;

static int read_segment(void *opaque, uint8_t *bytes, int size) {
    Input *input = opaque;
    if (input->out->cancelled(input->out->user)) return AVERROR_EXIT;
    size_t left = input->size - input->offset;
    if (!left) return AVERROR_EOF;
    size_t count = (size_t)size < left ? (size_t)size : left;
    memcpy(bytes, input->input + input->offset, count);
    input->offset += count;
    return (int)count;
}

static int64_t seek_buffer(void *opaque, int64_t offset, int mode) {
    Input *input = opaque;
    if (mode == AVSEEK_SIZE) return (int64_t)input->size;
    if (input->out->cancelled(input->out->user)) return AVERROR_EXIT;
    int64_t origin;
    switch (mode & ~AVSEEK_FORCE) {
        case SEEK_SET: origin = 0; break;
        case SEEK_CUR: origin = (int64_t)input->offset; break;
        case SEEK_END: origin = (int64_t)input->size; break;
        default: return AVERROR(EINVAL);
    }
    if (offset > (int64_t)input->size - origin || offset < -origin) return AVERROR(EINVAL);
    return (int64_t)(input->offset = (size_t)(offset + origin));
}

static int interrupt_decode(void *opaque) {
    const MediaOutput *out = opaque;
    return out->cancelled(out->user);
}

static int open_decoder(const AVStream *stream, enum AVCodecID allowed, AVCodecContext **out) {
    if (stream->codecpar->codec_id != allowed) return AVERROR_DECODER_NOT_FOUND;
    const AVCodec *codec = avcodec_find_decoder(allowed);
    if (!codec) return AVERROR_DECODER_NOT_FOUND;
    *out = avcodec_alloc_context3(codec);
    if (!*out) return AVERROR(ENOMEM);
    int result = avcodec_parameters_to_context(*out, stream->codecpar);
    if (result < 0) return result;
    (*out)->thread_count = 1;
    (*out)->max_pixels = (TWITCH_MAX_WIDTH + 32) * (TWITCH_MAX_HEIGHT + 32);
    (*out)->pkt_timebase = stream->time_base;
    return avcodec_open2(*out, codec, NULL);
}

CaptionDecoder *media_captions_open(void) {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_EIA_608);
    if (!codec) return NULL;
    CaptionDecoder *decoder = avcodec_alloc_context3(codec);
    if (!decoder) return NULL;
    decoder->pkt_timebase = (AVRational){1, 1000000};
    AVDictionary *options = NULL;
    av_dict_set(&options, "real_time", "1", 0);
    av_dict_set(&options, "data_field", "first", 0);
    int result = avcodec_open2(decoder, codec, &options);
    av_dict_free(&options);
    if (result < 0) avcodec_free_context(&decoder);
    return decoder;
}

void media_captions_close(CaptionDecoder **decoder) { avcodec_free_context(decoder); }

static void caption_text(const char *event, char out[CAPTION_BYTES]) {
    *out = 0;
    if (!event) return;
    /* AVSubtitle sends an ASS event with eight fields before the text. */
    for (unsigned field = 0; field < 8; ++field) {
        event = strchr(event, ',');
        if (!event) return;
        ++event;
    }
    size_t written = 0;
    while (*event && written + 4 < CAPTION_BYTES) {
        if (*event == '{') {
            const char *end = strchr(event, '}');
            if (end) { event = end + 1; continue; }
        }
        if (*event == '\\' && (event[1] == 'N' || event[1] == 'n' || event[1] == 'h')) {
            out[written++] = event[1] == 'h' ? ' ' : '\n';
            event += 2;
        } else {
            size_t bytes = 1;
            while (bytes < 4 && ((unsigned char)event[bytes] & 0xc0) == 0x80) ++bytes;
            memcpy(out + written, event, bytes);
            written += bytes;
            event += bytes;
        }
    }
    while (written && (out[written - 1] == ' ' || out[written - 1] == '\n')) --written;
    out[written] = 0;
}

void media_caption_packet(const MediaOutput *out, const uint8_t *data, size_t size, int64_t pts_us) {
    if (!out->captions || !out->caption || size < 3 || size > 4096 || size % 3) return;
    AVPacket *packet = av_packet_alloc();
    if (!packet) return;
    if (av_new_packet(packet, (int)size) >= 0) {
        memcpy(packet->data, data, size);
        packet->pts = pts_us;
        AVSubtitle subtitle = {0};
        int ready = 0;
        if (avcodec_decode_subtitle2(out->captions, &subtitle, &ready, packet) >= 0 && ready && subtitle.num_rects) {
            char text[CAPTION_BYTES];
            caption_text(subtitle.rects[subtitle.num_rects - 1]->ass, text);
            out->caption(out->user, text);
        }
        avsubtitle_free(&subtitle);
    }
    av_packet_free(&packet);
}

static int64_t time_us(int64_t pts, AVRational time_base) {
    return pts == AV_NOPTS_VALUE ? pts : av_rescale_q(pts, time_base, (AVRational){1, 1000000});
}

static int video_frame(Decoder *decoder, const MediaOutput *out, const AVFrame *frame) {
    int64_t pts = time_us(frame->best_effort_timestamp, decoder->video->pkt_timebase);
    const AVFrameSideData *cc = av_frame_get_side_data(frame, AV_FRAME_DATA_A53_CC);
    if (decoder->trim_before != AV_NOPTS_VALUE && pts != AV_NOPTS_VALUE && pts < decoder->trim_before) {
        if (cc) media_caption_packet(out, cc->data, cc->size, pts);
        return 0;
    }
    decoder->trim_started = true;
    if (frame->width < 1 || frame->height < 1 ||
        frame->width > TWITCH_MAX_WIDTH || frame->height > TWITCH_MAX_HEIGHT)
        return AVERROR_INVALIDDATA;
    const bool planar = frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P;
    const bool bt709 = frame->colorspace == AVCOL_SPC_BT709;
    const bool bt601 = frame->colorspace == AVCOL_SPC_UNSPECIFIED || frame->colorspace == AVCOL_SPC_BT470BG ||
                       frame->colorspace == AVCOL_SPC_SMPTE170M;
    if (out->video_yuv && planar && (bt709 || bt601)) {
        const bool full = frame->format == AV_PIX_FMT_YUVJ420P || frame->color_range == AVCOL_RANGE_JPEG;
        MediaYuvFrame native = {
            .width = frame->width, .height = frame->height,
            .format = bt709 ? (full ? MEDIA_709_FULL : MEDIA_709_LIMITED)
                            : (full ? MEDIA_601_FULL : MEDIA_601_LIMITED),
        };
        for (unsigned i = 0; i < 3; ++i) {
            native.plane[i] = frame->data[i];
            native.stride[i] = frame->linesize[i];
        }
        if (!out->video_yuv(out->user, &native, pts)) return AVERROR_EXIT;
        if (cc) media_caption_packet(out, cc->data, cc->size, pts);
        return 0;
    }
    size_t bytes = (size_t)frame->width * frame->height * 4 + 64;
    if (decoder->rgba_capacity < bytes) {
        uint8_t *buffer = av_realloc(decoder->rgba, bytes);
        if (!buffer) return AVERROR(ENOMEM);
        decoder->rgba = buffer;
        decoder->rgba_capacity = bytes;
    }
    decoder->scaler = sws_getCachedContext(decoder->scaler,
        frame->width, frame->height, frame->format, frame->width, frame->height, AV_PIX_FMT_RGBA,
        SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!decoder->scaler) return AVERROR(ENOMEM);
    uint8_t *destination[] = {decoder->rgba};
    const int stride[] = {frame->width * 4};
    if (sws_scale(decoder->scaler, (const uint8_t *const *)frame->data, frame->linesize,
            0, frame->height, destination, stride) != frame->height) return AVERROR_INVALIDDATA;
    if (!out->video(out->user, decoder->rgba, pts, frame->width, frame->height)) return AVERROR_EXIT;
    if (cc) media_caption_packet(out, cc->data, cc->size, pts);
    return 0;
}

static int audio_frame(Decoder *decoder, const MediaOutput *out, const AVFrame *frame) {
    if (frame->sample_rate < 32000 || frame->sample_rate > 96000 || frame->ch_layout.nb_channels < 1 ||
        frame->ch_layout.nb_channels > 2 || frame->nb_samples > 4096) return AVERROR_INVALIDDATA;
    if (!decoder->resampler) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        int rc = swr_alloc_set_opts2(&decoder->resampler, &stereo, AV_SAMPLE_FMT_S16, AUDIO_RATE,
            &frame->ch_layout, frame->format, frame->sample_rate, 0, NULL);
        if (rc < 0) return rc;
        rc = swr_init(decoder->resampler);
        if (rc < 0) return rc;
    }
    uint8_t *destination = (uint8_t *)decoder->pcm;
    int frames = swr_convert(decoder->resampler, &destination, 8192,
        (const uint8_t **)frame->extended_data, frame->nb_samples);
    if (frames < 0) return frames;
    int skip = 0;
    if (decoder->trim_before != AV_NOPTS_VALUE) {
        int64_t pts = time_us(frame->best_effort_timestamp, decoder->audio->pkt_timebase);
        if (pts == AV_NOPTS_VALUE && !decoder->trim_started) return 0;
        if (pts != AV_NOPTS_VALUE && pts < decoder->trim_before) {
            int64_t missing = av_rescale_rnd(decoder->trim_before - pts, AUDIO_RATE, 1000000, AV_ROUND_UP);
            if (missing >= frames) return 0;
            skip = (int)missing;
        }
    }
    return out->audio(out->user, decoder->pcm + skip * 2, (unsigned)(frames - skip)) ? 0 : AVERROR_EXIT;
}

static int consume(Decoder *decoder, AVCodecContext *codec, AVFrame *frame,
                   const AVPacket *packet, const MediaOutput *out) {
    int rc = avcodec_send_packet(codec, packet);
    if (rc < 0 && rc != AVERROR_EOF) return rc;
    while ((rc = avcodec_receive_frame(codec, frame)) >= 0) {
        rc = codec == decoder->video ? video_frame(decoder, out, frame) : audio_frame(decoder, out, frame);
        av_frame_unref(frame);
        if (rc < 0) return rc;
    }
    return rc == AVERROR(EAGAIN) || rc == AVERROR_EOF ? 0 : rc;
}

static int decode(const uint8_t *data, size_t size, const MediaOutput *out, bool mp4, bool clip) {
    Input input = {.input = data, .size = size, .out = out};
    Decoder decoder = {.trim_before = AV_NOPTS_VALUE};
    AVFormatContext *format = avformat_alloc_context();
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    uint8_t *buffer = av_malloc(32768);
    decoder.pcm = av_malloc(8192 * 4);
    AVIOContext *io = NULL;
    int rc = AVERROR(ENOMEM);
    if (!format || !frame || !packet || !buffer || !decoder.pcm) goto finish;
    io = avio_alloc_context(buffer, 32768, 0, &input, read_segment, NULL, mp4 ? seek_buffer : NULL);
    if (!io) goto finish;
    buffer = NULL;
    format->pb = io;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    format->interrupt_callback = (AVIOInterruptCB){interrupt_decode, (void *)out};
    format->probesize = 512 * 1024;
    format->max_analyze_duration = 600000;
    format->max_streams = 12;
    const AVInputFormat *demuxer = av_find_input_format(mp4 ? "mov" : "mpegts");
    if (!demuxer) { rc = AVERROR_DEMUXER_NOT_FOUND; goto finish; }
    rc = avformat_open_input(&format, NULL, demuxer, NULL);
    if (rc < 0) goto finish;
    rc = avformat_find_stream_info(format, NULL);
    if (rc < 0) goto finish;
    if (clip && out->clip_duration && format->duration > 0) {
        uint64_t ms = (uint64_t)format->duration / 1000;
        if (ms <= UINT32_MAX) out->clip_duration(out->user, (unsigned)ms);
    }
    int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int audio = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, video, NULL, 0);
    if (video < 0) { rc = video; goto finish; }
    if (!clip && out->skip_ms) {
        const AVStream *stream = format->streams[video];
        int64_t start = time_us(stream->start_time, stream->time_base);
        if (start == AV_NOPTS_VALUE) start = format->start_time;
        if (start != AV_NOPTS_VALUE) decoder.trim_before = start + (int64_t)out->skip_ms * 1000;
    }
    if ((rc = open_decoder(format->streams[video], AV_CODEC_ID_H264, &decoder.video)) < 0) goto finish;
    if (audio >= 0 && (rc = open_decoder(format->streams[audio], AV_CODEC_ID_AAC, &decoder.audio)) < 0) goto finish;
    while (!out->cancelled(out->user) && (rc = av_read_frame(format, packet)) >= 0) {
        AVCodecContext *codec = packet->stream_index == video ? decoder.video :
                                packet->stream_index == audio ? decoder.audio : NULL;
        if (codec) rc = consume(&decoder, codec, frame, packet, out);
        av_packet_unref(packet);
        if (rc < 0) goto finish;
    }
    if (out->cancelled(out->user)) rc = AVERROR_EXIT;
    if (rc == AVERROR_EOF) {
        rc = consume(&decoder, decoder.video, frame, NULL, out);
        if (rc == 0 && decoder.audio) rc = consume(&decoder, decoder.audio, frame, NULL, out);
    }

finish:
    avcodec_free_context(&decoder.video);
    avcodec_free_context(&decoder.audio);
    sws_freeContext(decoder.scaler);
    swr_free(&decoder.resampler);
    av_free(decoder.rgba);
    av_free(decoder.pcm);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avformat_close_input(&format);
    av_free(buffer);
    if (io) av_freep(&io->buffer);
    avio_context_free(&io);
    return rc;
}

int decode_segment(const uint8_t *data, size_t size, const MediaOutput *out) {
    return decode(data, size, out, false, false);
}

int decode_fragment(const uint8_t *data, size_t size, const MediaOutput *out) {
    return decode(data, size, out, true, false);
}

int decode_clip(const uint8_t *data, size_t size, const MediaOutput *out) {
    return decode(data, size, out, true, true);
}
