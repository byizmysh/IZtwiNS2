/* SPDX-License-Identifier: GPL-3.0-only */
#include "library.h"
#ifdef IZTWINS2_IZA
#include "../iza/native-storage.h"
#endif
#include "twitch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static unsigned find(const ChannelList *list, const char *login) {
    for (unsigned i = 0; i < list->count; ++i)
        if (!strcmp(list->items[i].login, login)) return i;
    return list->count;
}

bool library_contains(const ChannelList *list, const char *login) {
    return find(list, login) < list->count;
}

static void remove_at(ChannelList *list, unsigned index) {
    --list->count;
    memmove(&list->items[index], &list->items[index + 1], (list->count - index) * sizeof(list->items[0]));
}

static void prepend(ChannelList *list, const Channel *item) {
    if (list->count == CHANNEL_ROWS) --list->count;
    memmove(&list->items[1], &list->items[0], list->count * sizeof(list->items[0]));
    list->items[0] = *item;
    ++list->count;
}

bool library_toggle_favorite(ChannelLibrary *library, const Channel *channel) {
    unsigned at = find(&library->favorites, channel->login);
    if (at < library->favorites.count) {
        remove_at(&library->favorites, at);
        return false;
    }
    prepend(&library->favorites, channel);
    return true;
}

void library_add_recent(ChannelLibrary *library, const char *login) {
    Channel item = {0};
    snprintf(item.login, sizeof(item.login), "%s", login);
    unsigned index = find(&library->recent, login);
    if (index < library->recent.count) remove_at(&library->recent, index);
    prepend(&library->recent, &item);
}

static unsigned find_video(const ChannelList *list, const char *id) {
    for (unsigned i = 0; i < list->count; ++i)
        if (!strcmp(list->items[i].id, id)) return i;
    return list->count;
}

const Channel *library_resume(const ChannelLibrary *library, const char *video_id) {
    unsigned index = find_video(&library->resume, video_id);
    return index < library->resume.count ? &library->resume.items[index] : NULL;
}

bool library_remove_resume(ChannelLibrary *library, const char *video_id) {
    unsigned index = find_video(&library->resume, video_id);
    if (index == library->resume.count) return false;
    remove_at(&library->resume, index);
    return true;
}

bool library_set_resume(ChannelLibrary *library, const Channel *channel, unsigned position, unsigned duration) {
    if (!duration || channel->kind != ITEM_VIDEO || !twitch_video_id(channel->id)) return false;
    if (position >= duration || duration - position <= 3000)
        return library_remove_resume(library, channel->id);
    if (position < 5000) return library_remove_resume(library, channel->id);
    Channel item = *channel;
    const Channel *previous = library_resume(library, item.id);
    if (previous && previous->resume_ms == position && previous->duration_ms == duration) return false;
    if (previous && !*item.title) item = *previous;
    item.duration_ms = duration;
    item.resume_ms = position;
    library_remove_resume(library, item.id);
    prepend(&library->resume, &item);
    return true;
}

static bool unsigned_value(const char *text, unsigned *out) {
    if (*text < '0' || *text > '9') return false;
    errno = 0;
    char *end;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || *end || value > UINT32_MAX) return false;
    *out = (unsigned)value;
    return true;
}

static void load_resume(ChannelLibrary *library, char *line) {
    char *field[7] = {line};
    for (unsigned i = 1; i < 7; ++i) {
        char *separator = strchr(field[i - 1], '\t');
        if (!separator) return;
        *separator = 0;
        field[i] = separator + 1;
    }
    Channel item = {.kind = ITEM_VIDEO};
    if (strchr(field[6], '\t') || !twitch_video_id(field[0]) ||
        !unsigned_value(field[1], &item.resume_ms) || !unsigned_value(field[2], &item.duration_ms) ||
        item.resume_ms < 5000 || item.duration_ms <= item.resume_ms ||
        (*field[3] && !twitch_channel(field[3])) || library->resume.count == CHANNEL_ROWS ||
        library_resume(library, field[0])) return;
    char *targets[] = {item.id, item.login, item.name, item.title, item.thumbnail};
    const size_t caps[] = {sizeof(item.id), sizeof(item.login), sizeof(item.name), sizeof(item.title), sizeof(item.thumbnail)};
    const unsigned source[] = {0, 3, 4, 5, 6};
    for (unsigned i = 0; i < sizeof(source) / sizeof(*source); ++i) {
        size_t length = strlen(field[source[i]]);
        if (length >= caps[i]) return;
        memcpy(targets[i], field[source[i]], length + 1);
    }
    library->resume.items[library->resume.count++] = item;
}

static void load_text_sizes(ChannelLibrary *library, char *line) {
    char *separator = strchr(line, '\t');
    if (!separator) return;
    *separator = 0;
    unsigned captions, chat;
    if (unsigned_value(line, &captions) && unsigned_value(separator + 1, &chat)) {
        if (captions < TEXT_SIZE_COUNT) library->captions_size = (TextSize)captions;
        if (chat < TEXT_SIZE_COUNT) library->chat_size = (TextSize)chat;
    }
}

static void load_playback_settings(ChannelLibrary *library, char *line) {
    char *separator = strchr(line, '\t');
    if (!separator) return;
    *separator = 0;
    unsigned quality, captions;
    if (unsigned_value(line, &quality) && unsigned_value(separator + 1, &captions) &&
        (!quality || (quality >= 144 && quality <= TWITCH_MAX_HEIGHT)) && captions <= 1) {
        library->preferred_quality = quality;
        library->captions_on = captions != 0;
    }
}

bool library_load(ChannelLibrary *library, const char *path) {
#ifdef IZTWINS2_IZA
    FILE *file = iztwi_storage_read(path);
#else
    FILE *file = fopen(path, "r");
    if (!file && errno == ENOENT) {
        char backup[256];
        int n = snprintf(backup, sizeof(backup), "%s.bak", path);
        if (n > 0 && (size_t)n < sizeof(backup)) file = fopen(backup, "r");
    }
#endif
    if (!file) return false;
    char line[1152];
    while (fgets(line, sizeof(line), file)) {
        char *end = strchr(line, '\n');
        if (!end) {
            int ch;
            do { ch = fgetc(file); } while (ch != '\n' && ch != EOF);
            continue;
        }
        *end = 0;
        if (end > line && end[-1] == '\r') end[-1] = 0;
        if (line[1] != '\t') continue;
        if (line[0] == 'V') { load_resume(library, line + 2); continue; }
        if (line[0] == 'P') { load_text_sizes(library, line + 2); continue; }
        if (line[0] == 'Q') { load_playback_settings(library, line + 2); continue; }
        if (!twitch_channel(line + 2)) continue;
        ChannelList *list = line[0] == 'F' ? &library->favorites : line[0] == 'R' ? &library->recent : NULL;
        if (!list || list->count >= CHANNEL_ROWS || library_contains(list, line + 2)) continue;
        Channel *item = &list->items[list->count++];
        *item = (Channel){.kind = line[0] == 'F' ? ITEM_CHANNEL : ITEM_LIVE};
        memcpy(item->login, line + 2, strlen(line + 2) + 1);
    }
    fclose(file);
    return true;
}

#ifndef IZTWINS2_IZA
static bool side_path(char out[256], const char *path, const char *suffix) {
    int size = snprintf(out, 256, "%s.%s", path, suffix);
    return size > 0 && size < 256;
}

static bool remove_file(const char *path) { return !remove(path) || errno == ENOENT; }

static bool commit_file(const char *path, const char *temp, const char *backup, bool ok) {
    if (ok) {
        if (!rename(temp, path)) { remove_file(backup); return true; }
        /* Horizon SD rename needs a free destination. Keep the prior file
         * recoverable until the replacement has been committed. */
        if (errno == EEXIST && remove_file(backup) && !rename(path, backup)) {
            if (!rename(temp, path)) { remove(backup); return true; }
            rename(backup, path);
        }
    }
    remove(temp);
    return false;
}

#endif

bool library_save(const ChannelLibrary *library, const char *path) {
#ifdef IZTWINS2_IZA
    char *buffer = NULL;
    size_t size = 0;
    FILE *file = open_memstream(&buffer, &size);
#else
    char temp[256], backup[256];
    if (!side_path(temp, path, "tmp") || !side_path(backup, path, "bak")) return false;
    FILE *file = fopen(temp, "w");
#endif
    if (!file) return false;
    const ChannelList *lists[] = {&library->favorites, &library->recent};
    bool ok = true;
    for (unsigned i = 0; i < sizeof(lists) / sizeof(*lists); ++i)
        for (unsigned j = 0; j < lists[i]->count; ++j)
            if (fprintf(file, "%c\t%s\n", i == 0 ? 'F' : 'R', lists[i]->items[j].login) < 0) ok = false;
    for (unsigned i = 0; i < library->resume.count; ++i) {
        const Channel *item = &library->resume.items[i];
        if (fprintf(file, "V\t%s\t%u\t%u\t%s\t%s\t%s\t%s\n", item->id, item->resume_ms, item->duration_ms,
                    item->login, item->name, item->title, item->thumbnail) < 0) ok = false;
    }
    if (fprintf(file, "P\t%u\t%u\n", (unsigned)library->captions_size, (unsigned)library->chat_size) < 0) ok = false;
    if (fprintf(file, "Q\t%u\t%u\n", library->preferred_quality, (unsigned)library->captions_on) < 0) ok = false;
    if (fclose(file)) ok = false;
#ifdef IZTWINS2_IZA
    ok = ok && iztwi_storage_publish(path, buffer, size);
    free(buffer);
    return ok;
#else
    return commit_file(path, temp, backup, ok);
#endif
}

static bool refresh_token_valid(const char *token) {
    size_t size = strlen(token);
    if (!size || size >= TWITCH_TOKEN_BYTES) return false;
    return strspn(token, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~+/=") == size;
}

bool session_load(TwitchAccount *account, const char *path) {
#ifdef IZTWINS2_IZA
    FILE *file = iztwi_storage_read(path);
#else
    FILE *file = fopen(path, "r");
    if (!file && errno == ENOENT) {
        char backup[256];
        if (side_path(backup, path, "bak")) file = fopen(backup, "r");
    }
#endif
    if (!file) return false;
    char token[TWITCH_TOKEN_BYTES + 1] = "";
    bool ok = fgets(token, sizeof(token), file) && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    char *end = strchr(token, '\n');
    if (end) *end = 0;
    ok = ok && end && refresh_token_valid(token);
    if (ok) memcpy(account->refresh, token, strlen(token) + 1);
    volatile char *clear = token;
    for (size_t i = 0; i < sizeof(token); ++i) clear[i] = 0;
    return ok;
}

bool session_save(const TwitchAccount *account, const char *path) {
#ifdef IZTWINS2_IZA
    if (!refresh_token_valid(account->refresh)) return false;
    char token[TWITCH_TOKEN_BYTES + 1];
    int size = snprintf(token, sizeof(token), "%s\n", account->refresh);
    bool ok = size > 0 && (size_t)size < sizeof(token) && iztwi_storage_publish(path, token, size);
    volatile char *clear = token;
    for (size_t i = 0; i < sizeof(token); ++i) clear[i] = 0;
    return ok;
#else
    char temp[256], backup[256];
    if (!refresh_token_valid(account->refresh) || !side_path(temp, path, "tmp") ||
        !side_path(backup, path, "bak")) return false;
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    FILE *file = fdopen(fd, "w");
    if (!file) { close(fd); remove(temp); return false; }
    bool ok = fprintf(file, "%s\n", account->refresh) > 0;
    if (fclose(file)) ok = false;
    return commit_file(path, temp, backup, ok);
#endif
}

bool session_remove(const char *path) {
#ifdef IZTWINS2_IZA
    return iztwi_storage_remove(path);
#else
    char temp[256], backup[256];
    if (!side_path(temp, path, "tmp") || !side_path(backup, path, "bak")) return false;
    bool backups_cleared = remove_file(temp);
    backups_cleared &= remove_file(backup);
    return remove_file(path) && backups_cleared;
#endif
}
