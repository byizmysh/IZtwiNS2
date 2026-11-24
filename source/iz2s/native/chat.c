/* SPDX-License-Identifier: GPL-3.0-only */
#include "chat.h"
#include "account.h"
#include "https.h"
#include "twitch.h"

#include <stdio.h>
#include <string.h>

static void set_state(TwitchChat *chat, ChatState state) {
    mutexLock(&chat->lock);
    if (chat->state != state) { chat->state = state; ++chat->revision; }
    mutexUnlock(&chat->lock);
}

static void append(TwitchChat *chat, const ChatMessage *message) {
    mutexLock(&chat->lock);
    if (*message->id) {
        for (unsigned i = 0; i < chat->count; ++i) {
            if (!strcmp(message->id, chat->messages[(chat->next - 1 - i) % CHAT_LINES].id)) {
                mutexUnlock(&chat->lock);
                return;
            }
        }
    }
    chat->messages[chat->next++ % CHAT_LINES] = *message;
    if (chat->count < CHAT_LINES) ++chat->count;
    ++chat->revision;
    mutexUnlock(&chat->lock);
}

static void erase(char *value, size_t size) {
    volatile char *p = value;
    while (size--) *p++ = 0;
}

static bool irc_command(TwitchIrc *irc, const char *prefix, const char *value) {
    char line[TWITCH_TOKEN_BYTES + 32];
    int n = snprintf(line, sizeof(line), "%s%s\r\n", prefix, value);
    bool ok = n > 0 && (size_t)n < sizeof(line) && twitch_irc_write(irc, line, (size_t)n);
    erase(line, sizeof(line));
    return ok;
}

static bool login(TwitchChat *chat, TwitchIrc *irc) {
    /* The IRC WebSocket accepts one command per client text message. */
    char nickname[32];
    if (chat->authenticated) snprintf(nickname, sizeof(nickname), "%s", chat->login);
    else {
        unsigned value = (unsigned)armGetSystemTick() % 999999999u;
        snprintf(nickname, sizeof(nickname), "justinfan%u", value);
    }
    return irc_command(irc, "CAP REQ :", "twitch.tv/tags twitch.tv/commands") &&
        (chat->authenticated ? irc_command(irc, "PASS oauth:", chat->access) :
                               irc_command(irc, "PASS ", "SCHMOOPIIE")) &&
        irc_command(irc, "NICK ", nickname) && irc_command(irc, "JOIN #", chat->room);
}

static bool on_line(TwitchChat *chat, TwitchIrc *irc, const char *line) {
    if (!strncmp(line, "PING :", 6)) {
        char reply[256];
        int n = snprintf(reply, sizeof(reply), "PONG :%.235s\r\n", line + 6);
        return n > 0 && twitch_irc_write(irc, reply, (size_t)n);
    }
    if (!strncmp(line, ":tmi.twitch.tv RECONNECT", 24)) return false;
    if (!strncmp(line, ":tmi.twitch.tv 001 ", 19))
        set_state(chat, chat->authenticated ? CHAT_CONNECTED : CHAT_GUEST);
    if (!strncmp(line, ":tmi.twitch.tv NOTICE * :", 24) &&
        (strstr(line, "Login authentication failed") || strstr(line, "Improperly formatted auth"))) return false;
    ChatMessage message;
    if (chat_parse(line, chat->room, &message)) {
        set_state(chat, chat->authenticated ? CHAT_CONNECTED : CHAT_GUEST);
        append(chat, &message);
    }
    return true;
}

static bool send_helix(TwitchChat *chat, Https *net, ChatMessage *message, char error[128]) {
    char response[4096], body[CHAT_TEXT_BYTES * 2 + 160];
    unsigned code;
    if (!*chat->broadcaster) {
        char path[80];
        snprintf(path, sizeof(path), "/helix/users?login=%s", chat->room);
        if (!https_account(net, path, NULL, IZTWINS2_CLIENT_ID, chat->access, response, sizeof(response), &code)) return false;
        if (code != 200 || !parse_helix_user_id(response, chat->broadcaster)) {
            snprintf(error, 128, "%s", code == 401 ? "Sign in again to send messages." : "Twitch channel not found.");
            return false;
        }
    }
    if (!chat_payload(chat->broadcaster, chat->sender, message->text, body, sizeof(body))) {
        snprintf(error, 128, "This message could not be encoded.");
        return false;
    }
    if (!https_account(net, "/helix/chat/messages", body, IZTWINS2_CLIENT_ID, chat->access,
                       response, sizeof(response), &code)) return false;
    if (code == 200) return parse_chat_delivery(response, message->id, error);
    snprintf(error, 128, "%s", code == 401 || code == 403 ? "Twitch denied chat access. Sign in again." :
        code == 429 ? "Too many messages. Try again shortly." : "Twitch could not send the message.");
    return false;
}

static void send_next(TwitchChat *chat) {
    ChatMessage message = {0};
    mutexLock(&chat->lock);
    if (chat->sending) memcpy(message.text, chat->draft, sizeof(message.text));
    mutexUnlock(&chat->lock);
    if (!*message.text) return;
    Https net = {0};
    char error[128] = "";
    bool delivered = https_begin(&net, &chat->stop) && send_helix(chat, &net, &message, error);
    https_end(&net);
    if (delivered) {
        snprintf(message.author, sizeof(message.author), "%s", chat->login);
        append(chat, &message);
    }
    mutexLock(&chat->lock);
    chat->sending = false;
    if (delivered) chat->draft[0] = 0;
    snprintf(chat->delivery, sizeof(chat->delivery), "%s", delivered ? "" :
        *error ? error : "Delivery unconfirmed. Your draft was kept.");
    ++chat->revision;
    mutexUnlock(&chat->lock);
}

static void session(TwitchChat *chat, TwitchIrc *irc) {
    if (!login(chat, irc)) return;
    char line[8192], chunk[2048];
    size_t used = 0;
    bool oversized = false;
    uint64_t deadline = armTicksToNs(armGetSystemTick()) + UINT64_C(17000000000);
    while (!atomic_load(&chat->stop)) {
        send_next(chat);
        int count = twitch_irc_read(irc, chunk, sizeof(chunk));
        if (count < 0) return;
        mutexLock(&chat->lock);
        bool waiting = chat->state == CHAT_CONNECTING;
        mutexUnlock(&chat->lock);
        if (waiting && armTicksToNs(armGetSystemTick()) > deadline) return;
        for (int i = 0; i < count; ++i) {
            char byte = chunk[i];
            if (byte == '\n') {
                if (!oversized && used && line[used - 1] == '\r') {
                    line[used - 1] = 0;
                    if (!on_line(chat, irc, line)) return;
                }
                used = 0;
                oversized = false;
            } else if (used < sizeof(line) && byte) line[used++] = byte;
            else oversized = true;
        }
    }
}

static void worker(void *opaque) {
    TwitchChat *chat = opaque;
    for (unsigned attempt = 0; attempt < 3 && !atomic_load(&chat->stop); ++attempt) {
        if (attempt) {
            set_state(chat, CHAT_CONNECTING);
            for (unsigned wait = 0; wait < attempt * 12 && !atomic_load(&chat->stop); ++wait)
                svcSleepThread(100000000);
        }
        if (atomic_load(&chat->stop)) break;
        char error[128] = "";
        TwitchIrc *irc = twitch_irc_open(&chat->stop, error, sizeof(error));
        if (irc) {
            uint64_t connected_at = armTicksToNs(armGetSystemTick());
            session(chat, irc);
            twitch_irc_close(irc);
            if (armTicksToNs(armGetSystemTick()) - connected_at >= UINT64_C(60000000000)) attempt = 0;
            snprintf(error, sizeof(error), "Twitch closed the chat session.");
        }
        mutexLock(&chat->lock);
        snprintf(chat->error, sizeof(chat->error), "%s", error);
        if (chat->sending) {
            chat->sending = false;
            snprintf(chat->delivery, sizeof(chat->delivery), "Chat disconnected. Your draft was kept.");
            ++chat->revision;
        }
        mutexUnlock(&chat->lock);
    }
    if (!atomic_load(&chat->stop)) set_state(chat, CHAT_UNAVAILABLE);
    atomic_store(&chat->finished, true);
}

bool chat_start(TwitchChat *chat, const char *room, const TwitchAccount *account) {
    if (chat->running) return true;
    if (strlen(room) >= sizeof(chat->room)) return false;
    bool same = !strcmp(chat->room, room) && !strcmp(chat->login, account->login);
    strcpy(chat->room, room);
    if (!twitch_channel(chat->room)) return false;
    if (!same) {
        chat->next = chat->count = 0;
        chat->draft[0] = chat->delivery[0] = chat->broadcaster[0] = 0;
    }
    chat->sending = false;
    chat->error[0] = 0;
    chat->authenticated = *account->access && account->irc_read;
    chat->can_write = chat->authenticated && account->chat_write && *account->id;
    strcpy(chat->login, account->login);
    if (chat->authenticated) {
        strcpy(chat->sender, account->id);
        strcpy(chat->access, account->access);
    }
    atomic_store(&chat->stop, false);
    atomic_store(&chat->finished, false);
    set_state(chat, CHAT_CONNECTING);
    if (R_FAILED(threadCreate(&chat->thread, worker, chat, NULL, 0x30000, 0x30, -2))) {
        erase(chat->access, sizeof(chat->access));
        set_state(chat, CHAT_UNAVAILABLE);
        return false;
    }
    if (R_FAILED(threadStart(&chat->thread))) {
        threadClose(&chat->thread);
        erase(chat->access, sizeof(chat->access));
        set_state(chat, CHAT_UNAVAILABLE);
        return false;
    }
    chat->running = true;
    return true;
}

void chat_request_stop(TwitchChat *chat) {
    if (chat->running) atomic_store(&chat->stop, true);
}

void chat_stop(TwitchChat *chat) {
    if (chat->running) {
        chat_request_stop(chat);
        threadWaitForExit(&chat->thread);
        threadClose(&chat->thread);
        chat->running = false;
    }
    erase(chat->access, sizeof(chat->access));
    set_state(chat, CHAT_IDLE);
}

bool chat_reap(TwitchChat *chat) {
    if (!chat->running || !atomic_load(&chat->stop) || !atomic_load(&chat->finished)) return false;
    chat_stop(chat);
    return true;
}

bool chat_queue(TwitchChat *chat, const char *message) {
    if (!chat_message_valid(message)) return false;
    mutexLock(&chat->lock);
    bool ok = !chat->sending && !atomic_load(&chat->stop) && chat->state == CHAT_CONNECTED && chat->can_write;
    if (!chat->sending) {
        snprintf(chat->draft, sizeof(chat->draft), "%s", message);
        chat->sending = ok;
        snprintf(chat->delivery, sizeof(chat->delivery), "%s", ok ? "" : "Chat disconnected. Your draft was kept.");
        ++chat->revision;
    }
    mutexUnlock(&chat->lock);
    return ok;
}

void chat_draft(TwitchChat *chat, char out[CHAT_TEXT_BYTES]) {
    mutexLock(&chat->lock);
    memcpy(out, chat->draft, CHAT_TEXT_BYTES);
    mutexUnlock(&chat->lock);
}

bool chat_snapshot(TwitchChat *chat, ChatSnapshot *out, unsigned *revision) {
    mutexLock(&chat->lock);
    if (*revision == chat->revision) {
        mutexUnlock(&chat->lock);
        return false;
    }
    out->state = chat->state;
    out->can_send = chat->state == CHAT_CONNECTED && chat->can_write && !chat->sending;
    out->sending = chat->sending;
    snprintf(out->error, sizeof(out->error), "%s", chat->error);
    snprintf(out->delivery, sizeof(out->delivery), "%s", chat->delivery);
    out->count = chat->count;
    out->next = chat->next;
    uint64_t first = chat->next - chat->count;
    for (unsigned i = 0; i < chat->count; ++i) out->messages[i] = chat->messages[(first + i) % CHAT_LINES];
    *revision = chat->revision;
    mutexUnlock(&chat->lock);
    return true;
}
