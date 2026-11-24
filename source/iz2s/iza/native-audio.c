/* SPDX-License-Identifier: GPL-3.0-only */
#include "native-audio.h"
#include <errno.h>
#include <string.h>
#include <switch.h>

static const IzNativeAudio *host;
static uint64_t device[8];
static bool open, started, muted, flushing;
enum { BUFFER_COUNT = 6 };
static AudioOutBuffer descriptors[BUFFER_COUNT];
static AudioOutBuffer *owners[BUFFER_COUNT];

static uint64_t call(unsigned operation, const void *argument) {
    return host->functions[operation]((uintptr_t)device, (uintptr_t)argument, 0, 0);
}

void iztwi_audio_bind(const IzNativeAudio *audio) { host = audio; }

Result audoutInitialize(void) {
    if (!host) return ENOSYS;
    if (open) {
        uint32_t previous = iztwi_audio_finish();
        if (previous) return previous == IZ_NATIVE_STOP_PENDING ? EBUSY : previous;
    }
    uint64_t parameters = 0;
    uint64_t result = host->functions[IZ_AUDIO_INITIALIZE]((uintptr_t)&parameters, 0, 0, 0);
    if (!result) result = call(IZ_AUDIO_OPEN, &parameters);
    if (result) return (Result)result;
    open = true;
    muted = flushing = false;
    return 0;
}

u32 audoutGetSampleRate(void) { return (u32)call(IZ_AUDIO_RATE, NULL); }
u32 audoutGetChannelCount(void) { return (u32)call(IZ_AUDIO_CHANNELS, NULL); }

Result audoutAppendAudioOutBuffer(AudioOutBuffer *buffer) {
    unsigned slot = 0;
    while (slot < BUFFER_COUNT && owners[slot]) ++slot;
    if (slot == BUFFER_COUNT) return EBUSY;
    if (muted) memset(buffer->buffer, 0, buffer->data_size);
    AudioOutBuffer *native = &descriptors[slot];
    uint64_t result = host->functions[IZ_AUDIO_BUFFER]((uintptr_t)native, (uintptr_t)buffer->buffer,
                                                       buffer->buffer_size, buffer->data_size);
    if (result) return (Result)result;
    result = call(IZ_AUDIO_APPEND, native);
    if (!result) return EIO;
    owners[slot] = buffer;
    return result == 1 ? 0 : (Result)result;
}

Result audoutStartAudioOut(void) {
    Result result = (Result)call(IZ_AUDIO_START, NULL);
    if (!result) started = true;
    return result;
}

Result audoutStopAudioOut(void) {
    if (!started) return 0;
    Result result = (Result)call(IZ_AUDIO_STOP, NULL);
    if (!result) started = false;
    return result;
}

Result audoutFlushAudioOutBuffers(bool *flushed) {
    uint64_t result = call(IZ_AUDIO_FLUSH, NULL);
    if (flushed) *flushed = result == 1;
    if (result <= 1) flushing = true;
    return result > 1 ? (Result)result : 0;
}

Result audoutGetReleasedAudioOutBuffer(AudioOutBuffer **buffer, u32 *count) {
    *buffer = NULL;
    *count = 0;
    const uint64_t released = call(IZ_AUDIO_RELEASED, NULL);
    if (!released) return call(IZ_AUDIO_COUNT, NULL) == UINT32_MAX ? EIO : 0;
    for (unsigned i = 0; i < BUFFER_COUNT; ++i) {
        if (released == (uintptr_t)&descriptors[i] && owners[i]) {
            *buffer = owners[i];
            owners[i] = NULL;
            *count = 1;
            return 0;
        }
    }
    return EIO;
}

Result audoutGetAudioOutBufferCount(u32 *count) {
    *count = (u32)call(IZ_AUDIO_COUNT, NULL);
    return *count == UINT32_MAX ? EIO : 0;
}

Result audoutSetAudioOutVolume(float volume) {
    muted = volume <= 0;
    return 0;
}

uint32_t iztwi_audio_finish(void) {
    if (!open) return 0;
    uint32_t result = audoutStopAudioOut();
    if (result) return result;
    if (!flushing && (result = audoutFlushAudioOutBuffers(NULL))) return result;
    for (unsigned i = 0; i < BUFFER_COUNT; ++i) {
        AudioOutBuffer *buffer;
        u32 released;
        result = audoutGetReleasedAudioOutBuffer(&buffer, &released);
        if (result) return result;
        if (!released) break;
    }
    u32 pending;
    result = audoutGetAudioOutBufferCount(&pending);
    if (result) return result;
    if (pending) return IZ_NATIVE_STOP_PENDING;
    for (unsigned i = 0; i < BUFFER_COUNT; ++i)
        if (owners[i]) return IZ_NATIVE_STOP_PENDING;
    result = (uint32_t)call(IZ_AUDIO_CLOSE, NULL);
    if (!result) open = false;
    return result;
}

void audoutExit(void) {
    (void)iztwi_audio_finish();
}
