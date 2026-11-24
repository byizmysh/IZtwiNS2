/* SPDX-License-Identifier: GPL-3.0-only */
#include "engine.h"
#include "startup-note.h"
#include <stdint.h>

typedef struct {
    char path[80];
    uint64_t ticket;
    IzNativeImage image;
    bool live, releasing;
} Frontend;

static void report(IzNativeApp *app, const char *stage, uint32_t result) {
    startup_note(app, "startup-frontend.txt", stage, result);
}

__attribute__((section(".text.iz_app")))
uint32_t iz_app(IzNativeApp *app, uint32_t event) {
    if (event == IZ_NATIVE_INIT && app && app->bytes >= sizeof(*app))
        report(app, "frontend-v0110", 0);
    if (!sb4ModuleCompat(app) || !app->data || app->data_bytes < sizeof(Frontend))
        return event == IZ_NATIVE_STOP ? 0 : 0xe870;
    Frontend *state = app->data;
    const IzNativeApp4 *host = (const IzNativeApp4 *)app;
    if (event == IZ_NATIVE_INIT) {
        if (!host->image_begin || !host->image_step || !host->image_release) {
            report(app, "missing-image-api", 0xe870);
            return 0xe870;
        }
        uint32_t result = host->image_begin(app, state->path, &state->ticket);
        report(app, "image-begin", result);
        return result;
    }
    if (event == IZ_NATIVE_STOP) {
        if (!state->ticket) return 0;
        if (state->live) {
            uint32_t result = ((EngineEvent)state->image.entry)(app, event);
            if (result) return result;
            state->live = false;
        }
        if (!state->releasing) {
            uint32_t result = host->image_release(app, state->ticket);
            if (result) return result;
            state->releasing = true;
        }
        IzNativeImageProgress progress = {0};
        uint32_t result = host->image_step(app, state->ticket, 32, &progress, &state->image);
        if (result) return result;
        if (progress.state != IZ_NATIVE_IMAGE_CLOSED && progress.state != IZ_NATIVE_IMAGE_FAILED)
            return IZ_NATIVE_STOP_PENDING;
        state->ticket = 0;
        return 0;
    }
    if (event != IZ_NATIVE_FRAME) return 0xe870;
    if (state->live) return ((EngineEvent)state->image.entry)(app, event);
    IzNativeImageProgress progress = {0};
    uint32_t result = host->image_step(app, state->ticket, 32, &progress, &state->image);
    if (progress.state == IZ_NATIVE_IMAGE_FAILED && progress.done) state->ticket = 0;
    if (result || progress.error) {
        uint32_t failure = result ? result : progress.error;
        char stage[] = "image-load-stage-00";
        stage[17] += (char)(progress.stage / 10 % 10);
        stage[18] += (char)(progress.stage % 10);
        report(app, stage, failure);
        return failure;
    }
    if (progress.state != IZ_NATIVE_IMAGE_READY) return 0;
    if (!state->image.entry) {
        report(app, "missing-engine-entry", 0xe870);
        return 0xe870;
    }
    state->live = true;
    report(app, "engine-entry", 0);
    result = ((EngineEvent)state->image.entry)(app, IZ_NATIVE_INIT);
    report(app, "engine-return", result);
    return result;
}
