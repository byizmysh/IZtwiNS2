/* SPDX-License-Identifier: GPL-3.0-only */
#include <pthread.h>
#include <sb4/compat.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <switch/kernel/svc.h>

extern const uint8_t __tls_start[], __tls_end[], __tls_initialized[];

typedef struct TlsBlock {
    struct TlsBlock *next;
    _Alignas(16) uint8_t payload[];
} TlsBlock;

static const Sb4Compat *api;
static pthread_key_t key;
static _Atomic(TlsBlock *) allocations;
static _Thread_local volatile uint32_t tls_check = 0x495a5457u;

uint32_t iztwi_tls_init(const Sb4Compat *host) {
    int result = pthread_key_create(&key, NULL);
    if (!result) {
        api = host;
        if (tls_check != 0x495a5457u) return 0xe870;
    }
    return result;
}

uint32_t iztwi_tls_close(void) {
    int result = pthread_key_delete(key);
    if (result) return result;
    TlsBlock *block = atomic_exchange(&allocations, NULL);
    while (block) {
        TlsBlock *next = block->next;
        api->release(block);
        block = next;
    }
    api = NULL;
    return 0;
}

void *iztwi_tls_pointer(void) {
    TlsBlock *block = pthread_getspecific(key);
    if (block) return block->payload;
    const size_t bytes = (uintptr_t)__tls_end - (uintptr_t)__tls_start;
    const size_t initialized = (uintptr_t)__tls_initialized - (uintptr_t)__tls_start;
    if (!api || initialized > bytes || bytes > 65536 ||
        api->allocate_aligned((void **)&block, 16, sizeof(*block) + 16 + bytes) || !block)
        svcExitThread();
    memset(block->payload, 0, 16 + bytes);
    memcpy(block->payload + 16, __tls_start, initialized);
    if (pthread_setspecific(key, block)) {
        api->release(block);
        svcExitThread();
    }
    block->next = atomic_load(&allocations);
    while (!atomic_compare_exchange_weak(&allocations, &block->next, block)) {}
    return block->payload;
}

/* The image does not own the launcher process. */
void __wrap___nx_exit(void) { svcExitThread(); }
void __wrap_exit(int status) { (void)status; svcExitThread(); }
