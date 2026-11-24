/* SPDX-License-Identifier: GPL-3.0-only
 * libounce socket and TLS lifecycle adapted from IZ2S Cheese (MIT,
 * Copyright 2026 IZMYSH contributors). See the accompanying license.
 */
#include "https.h"
#include "twitch.h"

#include <switch.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

enum { IO_MS = 3000, RESPONSE_MS = 12000 };

static Mutex network_lock;
static unsigned network_users;
static bool socket_owned;

typedef struct {
    Https *owner;
    int fd;
    SslContext context;
    SslConnection ssl;
    uint64_t deadline;
    char buffer[8192];
    size_t read, used;
} Reader;

static uint64_t ms(void) { return armTicksToNs(armGetSystemTick()) / 1000000; }
static bool stopped(const Https *net) { return atomic_load(net->cancel); }

bool https_begin(Https *net, const atomic_bool *cancel) {
    *net = (Https){.cancel = cancel};
    mutexLock(&network_lock);
    if (!network_users) {
        Result rc = socketInitializeDefault();
        socket_owned = R_SUCCEEDED(rc);
        if (!socket_owned && rc != MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized)) {
            snprintf(net->error, sizeof(net->error), "Could not start network (0x%08x)", rc);
            mutexUnlock(&network_lock);
            return false;
        }
#ifdef IZTWINS2_IZA
        /* Video, chat and subtitles can wait on independent SSL requests. */
        rc = sslInitialize(3);
        if (R_FAILED(rc)) rc = sslInitialize(1);
#else
        rc = sslInitialize(1);
#endif
        if (R_FAILED(rc)) {
            if (socket_owned) socketExit();
            snprintf(net->error, sizeof(net->error), "Could not start TLS (0x%08x)", rc);
            mutexUnlock(&network_lock);
            return false;
        }
    }
    ++network_users;
    net->tls = true;
    mutexUnlock(&network_lock);
    return true;
}

void https_end(Https *net) {
    if (!net->tls) return;
    mutexLock(&network_lock);
    if (!--network_users) {
        sslExit();
        if (socket_owned) socketExit();
    }
    net->tls = false;
    mutexUnlock(&network_lock);
}

static int dial(Https *net, const char *host, const char *port) {
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM, .ai_protocol = IPPROTO_TCP};
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(host, port, &hints, &addresses) || !addresses) {
        snprintf(net->error, sizeof(net->error), "DNS lookup failed for %.88s", host);
        return -1;
    }
    int fd = -1;
    unsigned attempts = 0;
    for (struct addrinfo *at = addresses; at && attempts++ < 3 && !stopped(net); at = at->ai_next) {
        fd = socket(at->ai_family, at->ai_socktype, at->ai_protocol);
        if (fd < 0) continue;
        bool ready = false;
        if (!fcntl(fd, F_SETFL, O_NONBLOCK)) {
            ready = connect(fd, at->ai_addr, at->ai_addrlen) == 0;
            if (!ready && errno == EINPROGRESS) {
                struct pollfd p = {.fd = fd, .events = POLLOUT};
                const uint64_t deadline = ms() + IO_MS;
                while (ms() < deadline && !stopped(net)) {
                    int rc = poll(&p, 1, 100);
                    if (rc == 0) continue;
                    int error;
                    socklen_t bytes = sizeof(error);
                    ready = rc > 0 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &bytes) && !error;
                    break;
                }
            }
        }
        if (ready && !fcntl(fd, F_SETFL, 0)) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0) snprintf(net->error, sizeof(net->error), "Connection failed for %.84s", host);
    return fd;
}

static bool connect_tls_port(Reader *r, const char *host, const char *port) {
    r->fd = dial(r->owner, host, port);
    if (r->fd < 0) return false;
    Result rc = sslCreateContext(&r->context, SslVersion_TlsV12 | SslVersion_TlsV13);
    if (R_SUCCEEDED(rc)) rc = sslContextCreateConnection(&r->context, &r->ssl);
    if (R_SUCCEEDED(rc)) rc = sslConnectionSetOption(&r->ssl, SslOptionType_DoNotCloseSocket, true);
    if (R_SUCCEEDED(rc)) {
        errno = 0;
        int extra = socketSslConnectionSetSocketDescriptor(&r->ssl, r->fd);
        if (extra >= 0 && extra != r->fd) close(extra);
        if (extra < 0 && errno != ENOENT) {
            snprintf(r->owner->error, sizeof(r->owner->error), "TLS socket attachment failed");
            return false;
        }
        rc = sslConnectionSetHostName(&r->ssl, host, (u32)strlen(host) + 1);
    }
    if (R_SUCCEEDED(rc)) rc = sslConnectionSetVerifyOption(&r->ssl,
        SslVerifyOption_PeerCa | SslVerifyOption_HostName | SslVerifyOption_DateCheck);
    if (R_SUCCEEDED(rc)) rc = sslConnectionSetIoTimeout(&r->ssl, IO_MS);
    if (R_SUCCEEDED(rc)) rc = sslConnectionDoHandshake(&r->ssl, NULL, NULL, NULL, 0);
    if (R_SUCCEEDED(rc)) return true;
    snprintf(r->owner->error, sizeof(r->owner->error), "TLS failed for %.75s (0x%08x)", host, rc);
    return false;
}

static bool connect_tls(Reader *r, const char *host) { return connect_tls_port(r, host, "443"); }

static void disconnect(Reader *r) {
    if (serviceIsActive(&r->ssl.s)) sslConnectionClose(&r->ssl);
    if (serviceIsActive(&r->context.s)) sslContextClose(&r->context);
    if (r->fd >= 0) close(r->fd);
}

static bool send_all(Reader *r, const char *data, size_t size) {
    while (size && !stopped(r->owner)) {
        u32 sent = 0;
        if (R_FAILED(sslConnectionWrite(&r->ssl, data, (u32)size, &sent)) || !sent || sent > size) return false;
        data += sent;
        size -= sent;
    }
    return !size;
}

static int read_tls(Reader *r, void *data, size_t capacity) {
    if (stopped(r->owner) || ms() > r->deadline) return -1;
    u32 count = 0;
    if (R_FAILED(sslConnectionRead(&r->ssl, data, (u32)capacity, &count)) || count > capacity) return -1;
    return (int)count;
}

static int fill(Reader *r) {
    if (stopped(r->owner) || ms() > r->deadline) return -1;
    if (r->read < r->used) return 1;
    int count = read_tls(r, r->buffer, sizeof(r->buffer));
    if (count < 0) return -1;
    r->read = 0;
    r->used = count;
    return count ? 1 : 0;
}

static bool get_line(Reader *r, char *out, size_t cap, size_t *budget) {
    for (size_t used = 0; used < cap && *budget; ++used) {
        if (fill(r) <= 0) return false;
        char ch = r->buffer[r->read++];
        --*budget;
        if (ch == '\n') {
            if (!used || out[used - 1] != '\r') return false;
            out[used - 1] = 0;
            return true;
        }
        if (!ch) return false;
        out[used] = ch;
    }
    return false;
}

static bool size_field(const char *text, int base, char delimiter, size_t *value) {
    if (*text == '-' || *text == '+' || *text == ' ' || *text == '\t') return false;
    char *end;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, base);
    if (end == text || (*end && *end != delimiter) || errno || parsed > SIZE_MAX) return false;
    *value = (size_t)parsed;
    return true;
}

static bool get_bytes(Reader *r, char *out, size_t size) {
    while (size) {
#ifdef IZTWINS2_IZA
        if (r->read == r->used && size >= sizeof(r->buffer)) {
            size_t chunk = size < 32768 ? size : 32768;
            const int count = read_tls(r, out, chunk);
            if (count <= 0) return false;
            out += count;
            size -= (size_t)count;
            continue;
        }
#endif
        if (fill(r) <= 0) return false;
        size_t available = r->used - r->read;
        if (available > size) available = size;
        memcpy(out, r->buffer + r->read, available);
        r->read += available;
        out += available;
        size -= available;
    }
    return true;
}

static bool get_response(Reader *r, unsigned *status, char *location, char *out, size_t limit,
                         size_t *used, bool error_body) {
    char line[8192];
    size_t budget = 65536, length = 0;
    bool counted = false, chunks = false, encoded = false;
    if (!get_line(r, line, sizeof(line), &budget) || strlen(line) < 12 || strncmp(line, "HTTP/1.", 7) ||
        sscanf(line + 9, "%3u", status) != 1) return false;
    for (;;) {
        if (!get_line(r, line, sizeof(line), &budget)) return false;
        if (!*line) break;
        char *value = strchr(line, ':');
        if (!value) return false;
        *value++ = 0;
        value += strspn(value, " \t");
        size_t n = strlen(value);
        while (n && (value[n - 1] == ' ' || value[n - 1] == '\t')) value[--n] = 0;
        if (!strcasecmp(line, "Location")) {
            if (*location || !n || n >= URL_MAX) return false;
            memcpy(location, value, n + 1);
        } else if (!strcasecmp(line, "Content-Length")) {
            if (counted || !size_field(value, 10, 0, &length)) return false;
            counted = true;
        } else if (!strcasecmp(line, "Transfer-Encoding")) {
            if (chunks || strcasecmp(value, "chunked")) return false;
            chunks = true;
        } else if (!strcasecmp(line, "Content-Encoding")) encoded |= strcasecmp(value, "identity") != 0;
    }
    if (*line || encoded || (counted && chunks)) return false;
    if (*status != 200 && (!error_body || *status < 400)) return true;
    if (counted) {
        if (length > limit || !get_bytes(r, out, length)) return false;
        *used = length;
    } else if (chunks) {
        do {
            if (!get_line(r, line, sizeof(line), &budget) || !size_field(line, 16, ';', &length) || length > limit - *used)
                return false;
            if (!length) {
                do { if (!get_line(r, line, sizeof(line), &budget)) return false; } while (*line);
                break;
            }
            if (!get_bytes(r, out + *used, length) || !get_line(r, line, sizeof(line), &budget) || *line) return false;
            *used += length;
        } while (true);
    } else {
        int rc;
        while ((rc = fill(r)) > 0) {
            length = r->used - r->read;
            if (length > limit - *used || !get_bytes(r, out + *used, length)) return false;
            *used += length;
        }
        if (rc < 0) return false;
    }
    out[*used] = 0;
    return true;
}

bool https_fetch(Https *net, const char *input, const char *post, char *out, size_t cap,
                 size_t *used, char *final_url) {
    char url[URL_MAX], redirect[URL_MAX], request[URL_MAX + 512], host[128];
    if (strlen(input) >= sizeof(url) || !cap) return false;
    strcpy(url, input);
    net->error[0] = 0;
    for (unsigned attempt = 0; attempt < 4 && !stopped(net); ++attempt) {
        const char *resource;
        if (!twitch_split_https(url, host, &resource) || (post && strcmp(host, "gql.twitch.tv"))) break;
        int size = snprintf(request, sizeof(request), "%s %s HTTP/1.1\r\nHost: %s\r\n"
            "User-Agent: IZtwiNS2/0.8.4 (Nintendo Switch 2)\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n"
            "Origin: %s\r\nReferer: %s/\r\n", post ? "POST" : "GET", resource, host,
            post ? "https://www.twitch.tv" : "https://player.twitch.tv",
            post ? "https://www.twitch.tv" : "https://player.twitch.tv");
        if (size < 0 || (size_t)size >= sizeof(request) - 2) break;
        if (post) {
            int headers = snprintf(request + size, sizeof(request) - (size_t)size,
                "Client-ID: kd1unb4b3q4t58fwlpcbzcbnm76a8fp\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n",
                strlen(post));
            if (headers < 0 || (size_t)headers >= sizeof(request) - (size_t)size - 2) break;
            size += headers;
        }
        memcpy(request + size, "\r\n", 2);
        size += 2;
        Reader r = {.owner = net, .fd = -1, .deadline = ms() + RESPONSE_MS};
        unsigned status = 0;
        *used = 0;
        *redirect = 0;
        bool ok = connect_tls(&r, host) && send_all(&r, request, (size_t)size) &&
                  (!post || send_all(&r, post, strlen(post))) && get_response(&r, &status, redirect, out, cap - 1, used, false);
        disconnect(&r);
        if (!ok) break;
        if (status == 200) {
            if (final_url) strcpy(final_url, url);
            return true;
        }
        if (!post && status >= 300 && status < 400 && *redirect && twitch_join_url(url, redirect, request)) {
            strcpy(url, request);
            continue;
        }
        snprintf(net->error, sizeof(net->error), "Twitch returned HTTP %u", status);
        return false;
    }
    if (!*net->error) snprintf(net->error, sizeof(net->error), "Request interrupted or invalid HTTPS response");
    return false;
}

static bool header_value(const char *text, const char *extra) {
    if (!text || !*text) return false;
    for (; *text; ++text) {
        unsigned char c = (unsigned char)*text;
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
            !(c >= '0' && c <= '9') && !strchr(extra, c)) return false;
    }
    return true;
}

bool https_account(Https *net, const char *path, const char *form, const char *client_id,
                   const char *access_token, char *out, size_t cap, unsigned *status) {
    char request[URL_MAX + 512], location[URL_MAX];
    bool oauth_post = form && (!strcmp(path, "/oauth2/device") || !strcmp(path, "/oauth2/token"));
    bool chat_post = form && !strcmp(path, "/helix/chat/messages");
    bool validate = !form && !strcmp(path, "/oauth2/validate");
    bool helix = chat_post || (!form && !strncmp(path, "/helix/", 7));
    if (!cap || (!oauth_post && !validate && !helix) || !header_value(client_id, "_-") ||
        (!oauth_post && !header_value(access_token, "-._~+/="))) return false;
    for (const char *p = path; *p; ++p)
        if ((unsigned char)*p < 33 || (unsigned char)*p > 126 || *p == '#') return false;

    const char *host = helix ? "api.twitch.tv" : "id.twitch.tv";
    int size = snprintf(request, sizeof(request), "%s %s HTTP/1.1\r\nHost: %s\r\n"
        "User-Agent: IZtwiNS2/0.8.4 (Nintendo Switch 2)\r\nAccept: application/json\r\n"
        "Accept-Encoding: identity\r\nConnection: close\r\nClient-ID: %s\r\n",
        form ? "POST" : "GET", path, host, client_id);
    if (size < 0 || (size_t)size >= sizeof(request)) return false;
    size_t space = sizeof(request) - (size_t)size;
    int extra = chat_post ? snprintf(request + size, space,
        "Authorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
        access_token, strlen(form)) : oauth_post ? snprintf(request + size, space,
        "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n\r\n", strlen(form)) :
        snprintf(request + size, space, "Authorization: %s %s\r\n\r\n",
                 validate ? "OAuth" : "Bearer", access_token);
    if (extra < 0 || (size_t)extra >= space) return false;

    Reader r = {.owner = net, .fd = -1, .deadline = ms() + RESPONSE_MS};
    *status = 0;
    *out = 0;
    *location = 0;
    size_t used = 0;
    net->error[0] = 0;
    bool ok = connect_tls(&r, host) && send_all(&r, request, (size_t)(size + extra)) &&
        (!form || send_all(&r, form, strlen(form))) && get_response(&r, status, location, out, cap - 1, &used, true);
    disconnect(&r);
    return ok;
}

bool https_image(Https *net, const char *url, char *out, size_t cap, size_t *used) {
    static const char cdn[] = "static-cdn.jtvnw.net";
    static const char clips[] = "clips-media-assets2.twitch.tv";
    if (strncmp(url, "https://", 8)) return false;
    const char *domain = url + 8;
    const char *host = !strncmp(domain, cdn, sizeof(cdn) - 1) ? cdn :
                       !strncmp(domain, clips, sizeof(clips) - 1) ? clips : NULL;
    if (!host) return false;
    const char *path = domain + strlen(host);
    char request[URL_MAX + 512], location[URL_MAX];
    if (!cap || *path != '/' || path[1] == '/') return false;
    for (const char *p = path; *p; ++p)
        if ((unsigned char)*p < 33 || (unsigned char)*p > 126 || *p == '#') return false;
    int size = snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: %s\r\n"
        "User-Agent: IZtwiNS2/0.8.4 (Nintendo Switch 2)\r\nAccept: image/jpeg, image/png\r\n"
        "Accept-Encoding: identity\r\nConnection: close\r\n\r\n", path, host);
    if (size < 0 || (size_t)size >= sizeof(request)) return false;
    Reader r = {.owner = net, .fd = -1, .deadline = ms() + RESPONSE_MS};
    unsigned status = 0;
    *used = 0;
    *location = 0;
    bool ok = connect_tls(&r, host) && send_all(&r, request, (size_t)size) &&
        get_response(&r, &status, location, out, cap - 1, used, false);
    disconnect(&r);
    return ok && status == 200;
}

enum { CHAT_WS_LIMIT = 65536, CHAT_WS_SEND = 1024 };
struct TwitchIrc { Https net; Reader wire; size_t remaining; bool fragmented; };

static void base64(const uint8_t *input, size_t size, char *out) {
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < size; i += 3) {
        unsigned a = input[i], b = i + 1 < size ? input[i + 1] : 0, c = i + 2 < size ? input[i + 2] : 0;
        *out++ = digits[a >> 2];
        *out++ = digits[(a & 3) << 4 | b >> 4];
        *out++ = i + 1 < size ? digits[(b & 15) << 2 | c >> 6] : '=';
        *out++ = i + 2 < size ? digits[c & 63] : '=';
    }
    *out = 0;
}

static bool websocket_handshake(Reader *r, const char *host) {
    uint8_t nonce[16], digest[20];
    char key[25], accept[29], joined[64], request[512], header[2048];
    randomGet(nonce, sizeof(nonce));
    base64(nonce, sizeof(nonce), key);
    snprintf(joined, sizeof(joined), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    sha1CalculateHash(digest, joined, strlen(joined));
    base64(digest, sizeof(digest), accept);
    int n = snprintf(request, sizeof(request),
        "GET / HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Origin: https://www.twitch.tv\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\n\r\n", host, key);
    size_t budget = 8192;
    unsigned status = 0;
    if (n <= 0 || (size_t)n >= sizeof(request) || !send_all(r, request, (size_t)n) ||
        !get_line(r, header, sizeof(header), &budget) || sscanf(header, "HTTP/1.1 %u", &status) != 1 ||
        status != 101) return false;
    bool upgraded = false, verified = false, connection = false;
    while (get_line(r, header, sizeof(header), &budget)) {
        if (!*header) return upgraded && verified && connection;
        char *value = strchr(header, ':');
        if (!value) return false;
        *value++ = 0;
        value += strspn(value, " \t");
        size_t len = strlen(value);
        while (len && (value[len - 1] == ' ' || value[len - 1] == '\t')) value[--len] = 0;
        if (!strcasecmp(header, "Upgrade")) upgraded = !strcasecmp(value, "websocket");
        else if (!strcasecmp(header, "Connection")) connection = !strcasecmp(value, "Upgrade");
        else if (!strcasecmp(header, "Sec-WebSocket-Accept")) verified = !strcmp(value, accept);
        else if (!strcasecmp(header, "Sec-WebSocket-Extensions")) return false;
    }
    return false;
}

static bool websocket_send(Reader *r, unsigned opcode, const char *text, size_t size) {
    uint8_t frame[CHAT_WS_SEND + 8], mask[4];
    if (size > CHAT_WS_SEND) return false;
    frame[0] = 0x80 | opcode;
    size_t header = 2;
    if (size < 126) frame[1] = 0x80 | size;
    else {
        frame[1] = 0x80 | 126;
        frame[2] = size >> 8;
        frame[3] = size;
        header = 4;
    }
    randomGet(mask, sizeof(mask));
    memcpy(frame + header, mask, sizeof(mask));
    header += sizeof(mask);
    for (size_t i = 0; i < size; ++i) frame[header + i] = (uint8_t)text[i] ^ mask[i % sizeof(mask)];
    bool ok = send_all(r, (const char *)frame, header + size);
    volatile uint8_t *clear = frame;
    for (size_t i = header; i < header + size; ++i) clear[i] = 0;
    return ok;
}

TwitchIrc *twitch_irc_open(const atomic_bool *cancel, char *error, size_t error_cap) {
    TwitchIrc *irc = calloc(1, sizeof(*irc));
    if (!irc) { snprintf(error, error_cap, "Not enough memory for chat."); return NULL; }
    irc->wire.owner = &irc->net;
    irc->wire.fd = -1;
    irc->wire.deadline = ms() + RESPONSE_MS;
    const char host[] = "irc-ws.chat.twitch.tv";
    if (!https_begin(&irc->net, cancel) || !connect_tls(&irc->wire, host) || !websocket_handshake(&irc->wire, host)) {
        snprintf(error, error_cap, "%s", *irc->net.error ? irc->net.error : "Twitch did not accept the chat connection.");
        twitch_irc_close(irc);
        return NULL;
    }
    return irc;
}

int twitch_irc_read(TwitchIrc *irc, char *data, size_t capacity) {
    Reader *r = &irc->wire;
    if (stopped(&irc->net) || !capacity) return -1;
    if (r->read == r->used) {
        s32 pending = 0;
        if (R_FAILED(sslConnectionPending(&r->ssl, &pending)) || !pending) {
            struct pollfd read = {.fd = r->fd, .events = POLLIN};
            int rc = poll(&read, 1, 120);
            if (!rc || (rc < 0 && errno == EINTR)) return 0;
            if (rc < 0 || !(read.revents & POLLIN)) return -1;
        }
    }
    r->deadline = ms() + IO_MS;
    if (!irc->remaining) {
        uint8_t header[2];
        if (!get_bytes(r, (char *)header, sizeof(header)) || (header[0] & 0x70) || (header[1] & 0x80)) return -1;
        const unsigned opcode = header[0] & 15;
        const bool finished = (header[0] & 0x80) != 0;
        uint64_t size = header[1] & 127;
        if (size >= 126) {
            uint8_t length[8];
            unsigned count = size == 126 ? 2 : 8;
            if (!get_bytes(r, (char *)length, count) || (count == 8 && (length[0] & 0x80))) return -1;
            size = 0;
            for (unsigned i = 0; i < count; ++i) size = (size << 8) | length[i];
        }
        if (opcode >= 8) {
            char command[125];
            if (!finished || size > sizeof(command) || opcode > 10 || !get_bytes(r, command, (size_t)size)) return -1;
            if (opcode == 8) { websocket_send(r, 8, command, (size_t)size); return -1; }
            if (opcode == 9 && !websocket_send(r, 10, command, (size_t)size)) return -1;
            return 0;
        }
        if (size > CHAT_WS_LIMIT || opcode != (irc->fragmented ? 0u : 1u)) return -1;
        irc->fragmented = !finished;
        irc->remaining = (size_t)size;
    }
    size_t count = irc->remaining < capacity ? irc->remaining : capacity;
    if (!get_bytes(r, data, count)) return -1;
    irc->remaining -= count;
    return (int)count;
}

bool twitch_irc_write(TwitchIrc *irc, const char *data, size_t size) {
    return websocket_send(&irc->wire, 1, data, size);
}

void twitch_irc_close(TwitchIrc *irc) {
    if (!irc) return;
    disconnect(&irc->wire);
    https_end(&irc->net);
    free(irc);
}
