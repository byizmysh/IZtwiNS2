/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <switch.h>

Result __real_socketInitialize(const SocketInitConfig *config);
Result __real_tmemClose(TransferMemory *memory);

/* The HTTPS network lock serializes these hooks. The engine may also reap
 * the pending buffer after all of its workers have joined. */
static void *retired_buffer;
static size_t retired_size;
static atomic_bool delayed_return;

bool iztwi_bsd_waited_for_memory(void) {
    return atomic_load(&delayed_return);
}

static bool buffer_returned(void) {
    uintptr_t next = (uintptr_t)retired_buffer;
    const uintptr_t end = next + retired_size;
    while (next < end) {
        MemoryInfo info;
        u32 page_info;
        if (R_FAILED(svcQueryMemory(&info, &page_info, next)) ||
            (info.perm & Perm_Rw) != Perm_Rw || (info.attr & MemAttr_IsBorrowed) ||
            info.addr > next || info.addr + info.size <= next)
            return false;
        next = info.addr + info.size;
    }
    return true;
}

bool iztwi_bsd_reap(void) {
    if (!retired_buffer) return true;
    const u64 start = armGetSystemTick();
    do {
        if (buffer_returned()) {
            free(retired_buffer);
            retired_buffer = NULL;
            return true;
        }
        atomic_store(&delayed_return, true);
        svcSleepThread(1000000);
    } while (armTicksToNs(armGetSystemTick() - start) < 250000000);
    return false;
}

Result __wrap_socketInitialize(const SocketInitConfig *config) {
    /* Keep at most one retired BSD allocation pending across requests. */
    if (!iztwi_bsd_reap()) return MAKERESULT(Module_Libnx, LibnxError_Timeout);
    return __real_socketInitialize(config);
}

Result __wrap_tmemClose(TransferMemory *memory) {
    if (!memory->src_addr || memory->perm != Perm_None || memory->map_addr)
        return __real_tmemClose(memory);

    /* Closing our handle does not wait for BSD to drop its copy. Freeing
     * allocator-owned pages while BSD still holds them can fault in free(). */
    const Result result = tmemCloseHandle(memory);
    retired_buffer = memory->src_addr;
    retired_size = memory->size;
    memory->src_addr = NULL;
    iztwi_bsd_reap();
    return result;
}
