/* SPDX-License-Identifier: GPL-3.0-only */
#include "engine.h"
#include "startup-note.h"
#include "native-audio.h"
#include "native-storage.h"
#include "network-imports.h"
#include "player-platform.h"
#include "../native/menu.h"

#include <errno.h>
#include <sb4/host-posix.h>
#include <sb4/host-runtime.h>
#include <switch.h>

extern const Sb4HostHook __preinit_array_start[], __preinit_array_end[];
extern const Sb4HostHook __init_array_start[], __init_array_end[];
extern const Sb4HostHook __fini_array_start[], __fini_array_end[];
void * __dso_handle;
void witchtubeHostedUnregisterUnwind(void);
uint32_t iztwi_tls_init(const Sb4Compat *host);
uint32_t iztwi_tls_close(void);
bool iztwi_bsd_reap(void);
bool iztwi_bsd_waited_for_memory(void);

#define DECLARE_IMPORT(name) static __typeof__(name) *host_##name;
IZTWINS2_HOST_IMPORTS(DECLARE_IMPORT)
#undef DECLARE_IMPORT

#define IMPORT_WRAPPER(result, name, params, args) result __wrap_##name params { return host_##name args; }
IMPORT_WRAPPER(Result, threadCreate,
               (Thread *t, ThreadFunc f, void *a, void *s, size_t n, int p, int c), (t, f, a, s, n, p, c))
IMPORT_WRAPPER(Result, threadStart, (Thread *t), (t))
IMPORT_WRAPPER(Result, threadClose, (Thread *t), (t))
IMPORT_WRAPPER(Result, threadWaitForExit, (Thread *t), (t))
IMPORT_WRAPPER(Result, swkbdCreate, (SwkbdConfig *c, s32 n), (c, n))
IMPORT_WRAPPER(Result, swkbdShow, (SwkbdConfig *c, char *s, size_t n), (c, s, n))
#undef IMPORT_WRAPPER
void __wrap_swkbdClose(SwkbdConfig *c) { host_swkbdClose(c); }
void __wrap_swkbdConfigMakePresetDefault(SwkbdConfig *c) { host_swkbdConfigMakePresetDefault(c); }
void __wrap_swkbdConfigSetGuideText(SwkbdConfig *c, const char *s) { host_swkbdConfigSetGuideText(c, s); }
void __wrap_swkbdConfigSetInitialText(SwkbdConfig *c, const char *s) { host_swkbdConfigSetInitialText(c, s); }

static bool runtime_ready, runtime_retained, tls_ready, sm_ready, player_ready, closing;
static PlayerDisplay display;
static bool first_frame, bsd_return_reported;

static void report(IzNativeApp *app, const char *stage, uint32_t result) {
    startup_note(app, "startup-engine.txt", stage, result);
}

static void report_bsd_return(IzNativeApp *app) {
    if (!bsd_return_reported && iztwi_bsd_waited_for_memory()) {
        bsd_return_reported = true;
        startup_note(app, "bsd-memory.txt", "waited-for-bsd-buffer", 0);
    }
}

static uint32_t initialize(IzNativeApp *app) {
    report(app, "engine-v0110", 0);
    const Sb4Compat *api = sb4ModuleCompat(app);
    const IzNativeAudio *audio = sb4ModuleAudio(app);
    const IzNativeApp2 *native = (const IzNativeApp2 *)app;
    if (!api || !audio || !native->private_memory || native->private_bytes < PRIVATE_BYTES) {
        report(app, "missing-audio-or-framebuffer", ENOSYS);
        return ENOSYS;
    }
#define RESOLVE_IMPORT(name)     host_##name = (__typeof__(name) *)api->resolve_c(#name);     if (!host_##name) { report(app, #name, ENOSYS); return ENOSYS; }
    IZTWINS2_HOST_IMPORTS(RESOLVE_IMPORT)
#undef RESOLVE_IMPORT

    uint32_t result = sb4HostPosixBind(api);
    report(app, "bind-posix", result);
    if (result) return result;
    result = iztwi_tls_init(api);
    report(app, "engine-tls", result);
    if (result) return result;
    tls_ready = true;

    const Sb4HostHooks hooks = {
        .preinit = __preinit_array_start, .preinit_count = __preinit_array_end - __preinit_array_start,
        .init = __init_array_start, .init_count = __init_array_end - __init_array_start,
        .fini = __fini_array_start, .fini_count = __fini_array_end - __fini_array_start,
    };
    result = sb4HostRuntimeInitialize(api, &hooks);
    report(app, "engine-runtime", result);
    if (result) {
        runtime_retained = result == SB4_HOST_RUNTIME_RETAINED;
        return result;
    }
    runtime_ready = true;

    /* The engine owns its public NIFM, BSD and TLS sessions. */
    result = smInitialize();
    report(app, "service-manager", result);
    if (result) return result;
    sm_ready = true;
    /* Hosted libounce selects the Switch 2 service ABI when no precise OS
       version is supplied. This app's sockets own transfer memory without
       mapping a local alias, and IZ2S owns the threads. */
    report(app, "runtime-ready", 0);

    _Static_assert(UI_OFFSET + UI_WIDTH * UI_HEIGHT * sizeof(uint32_t) <= VIDEO_OFFSET,
                   "UI and video regions cannot overlap");
    _Static_assert(VIDEO_OFFSET + TWITCH_MAX_WIDTH * TWITCH_MAX_HEIGHT * sizeof(uint32_t) <= PRIVATE_BYTES,
                   "Native video region must hold the largest rendition");
    uint8_t *memory = native->private_memory;
    display.surface = (VideoSurface *)memory;
    display.pixels = (uint32_t *)(memory + UI_OFFSET);
    display.video = (uint32_t *)(memory + VIDEO_OFFSET);
    iztwi_audio_bind(audio);
    app->flags |= IZ_NATIVE_MANAGED_INPUT;
    iztwi_player_start(iztwi_storage_bind(app), app->buttons(app));
    player_ready = true;
    report(app, "player-ready", 0);
    return 0;
}

static uint32_t stop_player(IzNativeApp *app) {
    if (runtime_retained) return SB4_HOST_RUNTIME_RETAINED;
    if (player_ready) {
        if (!closing) {
            closing = true;
            iztwi_player_request_stop();
        }
        if (!iztwi_player_workers_stopped()) return IZ_NATIVE_STOP_PENDING;
        iztwi_player_stop();
        player_ready = false;
        if (!iztwi_bsd_reap()) report(app, "bsd-buffer-still-borrowed", ETIMEDOUT);
        report_bsd_return(app);
    }
    uint32_t result = iztwi_audio_finish();
    if (result) return result;
    if (sm_ready) {
        smExit();
        sm_ready = false;
    }
    if (runtime_ready) {
        result = sb4HostPosixQuiesce();
        if (result) return result == EBUSY ? IZ_NATIVE_STOP_PENDING : result;
        result = sb4HostRuntimeFinalize();
        if (result) return result;
        witchtubeHostedUnregisterUnwind();
        runtime_ready = false;
    }
    if (!tls_ready) return 0;
    result = iztwi_tls_close();
    if (!result) tls_ready = false;
    return result;
}

__attribute__((section(".text.iz_engine")))
uint32_t iz_engine(IzNativeApp *app, uint32_t event) {
    switch (event) {
        case IZ_NATIVE_INIT: return initialize(app);
        case IZ_NATIVE_FRAME:
            if (!iztwi_player_frame(&display, app->buttons(app),
                                    touch_iz2s_contact(app->controls, app->controls_bytes)))
                app->request_exit(app);
            if (!first_frame) {
                first_frame = true;
                report(app, "first-frame-v0110", 0);
            }
            report_bsd_return(app);
            return 0;
        case IZ_NATIVE_STOP: return stop_player(app);
        default: return EINVAL;
    }
}
