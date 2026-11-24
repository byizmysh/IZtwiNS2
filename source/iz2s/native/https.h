#ifndef IZTWINS2_HTTPS_H
#define IZTWINS2_HTTPS_H
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool tls;
    const atomic_bool *cancel;
    char error[128];
} Https;

bool https_begin(Https *net, const atomic_bool *cancel);
void https_end(Https *net);
bool https_fetch(Https *net, const char *url, const char *post, char *out, size_t cap,
                 size_t *used, char *final_url);
/* Account requests use fixed Twitch origins and never follow redirects. */
bool https_account(Https *, const char *path, const char *form, const char *client_id,
                   const char *access_token, char *out, size_t cap, unsigned *status);
/* Stream previews go to Twitch's image CDN without account headers. */
bool https_image(Https *, const char *path, char *out, size_t cap, size_t *used);

typedef struct TwitchIrc TwitchIrc;
TwitchIrc *twitch_irc_open(const atomic_bool *cancel, char *error, size_t error_cap);
/* Negative is closed, zero is idle, positive is the number of bytes read. */
int twitch_irc_read(TwitchIrc *, char *data, size_t capacity);
bool twitch_irc_write(TwitchIrc *, const char *data, size_t bytes);
void twitch_irc_close(TwitchIrc *);

#endif
