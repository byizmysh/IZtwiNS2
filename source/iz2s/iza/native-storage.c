/* SPDX-License-Identifier: GPL-3.0-only */
#include "native-storage.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_DOCUMENT = 128 * 1024 };
static IzNativeApp *owner;
static const Sb4StorageFiles *files;
static Sb4StorageReplace replace_file;

typedef struct {
    uint32_t token;
    uint64_t offset;
} Reader;

bool iztwi_storage_bind(IzNativeApp *app) {
    owner = app;
    files = sb4StorageFileChanges(sb4ModuleStorage(app));
    replace_file = (Sb4StorageReplace)sb4ModuleCompat(app)->resolve_c("sb4ReplaceFile");
    return files && replace_file;
}

static int read_chunk(void *cookie, char *buffer, size_t bytes) {
    Reader *reader = cookie;
    uint32_t count;
    if (files->read(owner, reader->token, reader->offset, buffer, bytes, &count)) {
        errno = EIO;
        return -1;
    }
    reader->offset += count;
    return (int)count;
}

static int close_reader(void *cookie) {
    Reader *reader = cookie;
    bool failed = files->close(owner, reader->token) != 0;
    free(reader);
    return failed ? -1 : 0;
}

FILE *iztwi_storage_read(const char *path) {
    if (!files) return NULL;
    Reader *reader = calloc(1, sizeof(*reader));
    if (!reader) return NULL;
    uint64_t size;
    if (files->open(owner, path, SB4_STORAGE_OPEN_READ, &reader->token, &size)) {
        free(reader);
        return NULL;
    }
    FILE *file = size <= MAX_DOCUMENT ? funopen(reader, read_chunk, NULL, NULL, close_reader) : NULL;
    if (!file) close_reader(reader);
    return file;
}

bool iztwi_storage_publish(const char *path, const char *data, size_t size) {
    if (!files || !replace_file || !size || size > MAX_DOCUMENT) return false;
    char staging[80];
    int length = snprintf(staging, sizeof(staging), "%s.stage", path);
    if (length <= 0 || (size_t)length >= sizeof(staging)) return false;
    return !owner->write_file(owner, staging, data, (uint32_t)size) &&
           !replace_file(owner, staging, path);
}

bool iztwi_storage_remove(const char *path) {
    if (!files) return false;
    char staged[80], recovery[80];
    int stage_size = snprintf(staged, sizeof(staged), "%s.stage", path);
    int recovery_size = snprintf(recovery, sizeof(recovery), "%s.iz2s-replace-old", path);
    if (stage_size <= 0 || recovery_size <= 0 || (size_t)stage_size >= sizeof(staged) ||
        (size_t)recovery_size >= sizeof(recovery)) return false;
    const char *entries[] = {path, staged, recovery};
    bool ok = true;
    for (unsigned i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        uint32_t result = files->remove_file(owner, entries[i]);
        if (result && result != 0x202) ok = false;
    }
    return ok && !files->commit(owner);
}

void iztwi_storage_note(const char *path, const char *data, size_t size) {
    if (owner && owner->write_file && size && size <= MAX_DOCUMENT)
        owner->write_file(owner, path, data, (uint32_t)size);
}
