/* SPDX-License-Identifier: GPL-3.0-only
 * The device authorization flow follows app/src/api/twitch_auth.cpp.
 */
#include "account.h"
#include "https.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RESPONSE_BYTES = 192 * 1024 };
static const char scope[] = "user%3Aread%3Afollows%20chat%3Aread%20user%3Awrite%3Achat";
static const char client_id[] = IZTWINS2_CLIENT_ID;

static uint64_t seconds(void) { return armTicksToNs(armGetSystemTick()) / 1000000000; }
static bool stopped(const AccountJob *job) { return atomic_load(&job->cancelled); }

static void erase(void *data, size_t size) {
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}

void account_clear(TwitchAccount *account) { erase(account, sizeof(*account)); }

bool account_due(const TwitchAccount *account) {
    uint64_t now = seconds();
    if (!*account->access) return *account->refresh != 0;
    return now >= account->validate_at || now + 90 >= account->expires;
}

static void status(AccountJob *job, const char *text) {
    snprintf(job->status, sizeof(job->status), "%s", text);
}

static void bad_http(AccountJob *job, unsigned code) {
    if (code) snprintf(job->status, sizeof(job->status), "Twitch returned HTTP %u", code);
    else status(job, "Could not reach Twitch. Check your internet connection.");
}

static bool percent_encode(const char *text, char *out, size_t size) {
    const char *hex = "0123456789ABCDEF";
    size_t i = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        unsigned char c = *p;
        bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || strchr("-._~", c);
        if (size - i <= (plain ? 1u : 3u)) return false;
        if (plain) out[i++] = (char)c;
        else { out[i++] = '%'; out[i++] = hex[c >> 4]; out[i++] = hex[c & 15u]; }
    }
    if (i >= size) return false;
    out[i] = 0;
    return true;
}

static bool wait_for(AccountJob *job, unsigned delay, uint64_t deadline) {
    uint64_t end = seconds() + delay;
    while (!stopped(job)) {
        uint64_t now = seconds();
        if (now >= deadline) break;
        if (now >= end) return true;
        svcSleepThread(100000000);
    }
    return false;
}

static bool validate(AccountJob *job, Https *net, char *body) {
    unsigned code = 0, expires;
    bool ok = https_account(net, "/oauth2/validate", NULL, client_id,
                            job->account.access, body, RESPONSE_BYTES, &code);
    if (ok && code == 200 && parse_account_identity(body, client_id, &job->account, &expires)) {
        uint64_t now = seconds();
        job->account.expires = now + expires;
        job->account.validate_at = now + 3600;
        return true;
    }
    if (code == 401 || (ok && code == 200)) {
        account_clear(&job->account);
        status(job, "Your Twitch session expired. Sign in again.");
    } else bad_http(job, ok ? code : 0);
    return false;
}

static bool sign_in(AccountJob *job, Https *net, char *body) {
    char form[TWITCH_TOKEN_BYTES * 3 + 256];
    unsigned code = 0;
    snprintf(form, sizeof(form), "client_id=%s&scopes=%s", client_id, scope);
    if (!https_account(net, "/oauth2/device", form, client_id, NULL, body, RESPONSE_BYTES, &code) || code != 200) {
        bad_http(job, code);
        return false;
    }
    DeviceCode device;
    if (!parse_device_code(body, &device) || stopped(job)) {
        status(job, "Could not get a sign-in code.");
        return false;
    }
    snprintf(job->code, sizeof(job->code), "%s", device.code);
    snprintf(job->activation_link, sizeof(job->activation_link), "%s", device.link);
    atomic_store(&job->code_ready, true);
    uint64_t deadline = seconds() + device.expires;
    char encoded[TWITCH_TOKEN_BYTES * 3];
    bool encoded_ok = percent_encode(device.device, encoded, sizeof(encoded));
    erase(&device.device, sizeof(device.device));
    if (!encoded_ok) return false;
    snprintf(form, sizeof(form), "client_id=%s&scopes=%s&device_code=%s"
        "&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code", client_id, scope, encoded);
    erase(encoded, sizeof(encoded));
    bool activated = false;
    unsigned delay = device.interval, failures = 0;
    status(job, "This code expired. Request a new one.");
    while (wait_for(job, delay, deadline)) {
        unsigned expires;
        if (!https_account(net, "/oauth2/token", form, client_id, NULL, body, RESPONSE_BYTES, &code)) {
            if (++failures < 3) continue;
            bad_http(job, 0);
            break;
        }
        failures = 0;
        if (code == 200 && parse_account_tokens(body, &job->account, &expires)) {
            activated = validate(job, net, body);
            break;
        }
        char error[64] = "";
        parse_oauth_error(body, error);
        if (!strcmp(error, "authorization_pending")) continue;
        if (!strcmp(error, "slow_down") || code == 429) { delay += 5; continue; }
        if (!strcmp(error, "access_denied")) status(job, "Sign-in was canceled on Twitch.");
        else if (!strcmp(error, "invalid device code") || !strcmp(error, "expired_token"))
            status(job, "This code expired. Request a new one.");
        else bad_http(job, code);
        break;
    }
    erase(form, sizeof(form));
    if (!activated) account_clear(&job->account);
    return activated;
}

static bool maintain(AccountJob *job, Https *net, char *body) {
    if (!*job->account.access && !*job->account.refresh) return false;
    if (!*job->account.access || seconds() + 90 >= job->account.expires) {
        char encoded[TWITCH_TOKEN_BYTES * 3], form[TWITCH_TOKEN_BYTES * 3 + 128];
        unsigned code = 0, expires;
        if (!percent_encode(job->account.refresh, encoded, sizeof(encoded))) return false;
        snprintf(form, sizeof(form), "client_id=%s&grant_type=refresh_token&refresh_token=%s", client_id, encoded);
        erase(encoded, sizeof(encoded));
        bool ok = https_account(net, "/oauth2/token", form, client_id, NULL, body, RESPONSE_BYTES, &code);
        erase(form, sizeof(form));
        if (!ok) { bad_http(job, 0); return false; }
        if (code == 400 || code == 401) {
            account_clear(&job->account);
            status(job, "Your Twitch session expired. Sign in again.");
            return false;
        }
        if (code != 200 || !parse_account_tokens(body, &job->account, &expires)) {
            bad_http(job, code);
            return false;
        }
        job->account.expires = seconds() + expires;
        job->account.validate_at = 0;
    }
    return !account_due(&job->account) || validate(job, net, body);
}

static bool load_channels(AccountJob *job, Https *net, char *body) {
    char path[CATALOG_CURSOR_BYTES * 3 + 128], cursor[CATALOG_CURSOR_BYTES * 3];
    if (!percent_encode(job->cursor, cursor, sizeof(cursor))) return false;
    snprintf(path, sizeof(path), "/helix/streams/followed?first=30&user_id=%s%s%s", job->account.id,
             *cursor ? "&after=" : "", cursor);

    unsigned code = 0;
    bool ok = https_account(net, path, NULL, client_id, job->account.access, body, RESPONSE_BYTES, &code);
    if (ok && code == 200 && parse_channel_list(body, false, &job->rows)) return true;
    if (ok && code == 401 && !validate(job, net, body)) return false;
    if (ok && code == 200) status(job, "Twitch returned an invalid channel list.");
    else bad_http(job, ok ? code : 0);
    return false;
}

static bool load_public(AccountJob *job, Https *net, char *body) {
    PublicFeed feed;
    switch (job->action) {
        case ACCOUNT_POPULAR: feed = FEED_POPULAR; break;
        case ACCOUNT_TRENDS: feed = FEED_TRENDS; break;
        case ACCOUNT_SEARCH: feed = FEED_SEARCH; break;
        case ACCOUNT_CATEGORY: feed = FEED_CATEGORY; break;
        case ACCOUNT_CHANNEL: feed = FEED_CHANNEL; break;
        case ACCOUNT_VIDEOS: feed = FEED_VIDEOS; break;
        case ACCOUNT_CLIPS: feed = FEED_CLIPS; break;
        case ACCOUNT_FAVORITES: feed = FEED_FAVORITES; break;
        default: return false;
    }
    char request[4096];
    size_t size;
    if (!directory_request(feed, job->query, job->cursor, request, sizeof(request)) ||
        !https_fetch(net, "https://gql.twitch.tv/gql", request, body, RESPONSE_BYTES, &size, NULL)) {
        status(job, "Could not load the Twitch directory.");
        return false;
    }
    if (parse_directory(body, feed, &job->rows, &job->profile)) return true;
    status(job, feed == FEED_CHANNEL ? "This channel is unavailable." : "Twitch returned no results for this section.");
    return false;
}

static void worker(void *opaque) {
    AccountJob *job = opaque;
    char *body = malloc(RESPONSE_BYTES);
    Https net = {0};
    if (!body) status(job, "Not enough memory to load the Twitch directory.");
    else if (!https_begin(&net, &job->cancelled)) bad_http(job, 0);
    else if (job->action == ACCOUNT_SIGN_IN) job->success = sign_in(job, &net, body);
    else if (job->action == ACCOUNT_CHECK || job->action == ACCOUNT_FOLLOWING) {
        if (maintain(job, &net, body)) job->success = job->action == ACCOUNT_CHECK || load_channels(job, &net, body);
    } else job->success = load_public(job, &net, body);
    https_end(&net);
    if (body) { erase(body, RESPONSE_BYTES); free(body); }
    atomic_store(&job->finished, true);
}

bool account_start(AccountJob *job, AccountAction action, const TwitchAccount *account, const char *query, const char *cursor) {
    if (job->running) return false;
    job->success = false;
    job->status[0] = job->code[0] = job->activation_link[0] = 0;
    job->rows.count = 0;
    job->rows.next[0] = 0;
    job->action = action;
    job->account = *account;
    snprintf(job->query, sizeof(job->query), "%s", query ? query : "");
    snprintf(job->cursor, sizeof(job->cursor), "%s", cursor ? cursor : "");
    atomic_store(&job->cancelled, false);
    atomic_store(&job->finished, false);
    atomic_store(&job->code_ready, false);
    if (R_FAILED(threadCreate(&job->thread, worker, job, NULL, 0x40000, 0x2c, -2))) {
        account_clear(&job->account);
        return false;
    }
    if (R_FAILED(threadStart(&job->thread))) {
        threadClose(&job->thread);
        account_clear(&job->account);
        return false;
    }
    job->running = true;
    return true;
}

void account_join(AccountJob *job) {
    if (!job->running) return;
    threadWaitForExit(&job->thread);
    threadClose(&job->thread);
    job->running = false;
}
