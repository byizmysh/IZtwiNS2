/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef IZTWINS2_ACCOUNT_H
#define IZTWINS2_ACCOUNT_H

#include "account-types.h"
#include <stdatomic.h>
#include <switch.h>

#ifndef IZTWINS2_CLIENT_ID
#define IZTWINS2_CLIENT_ID "1a0hyv0sxnwpkvde7pjyo4gpwcnwj9"
#endif

typedef enum { ACCOUNT_SIGN_IN, ACCOUNT_FOLLOWING, ACCOUNT_POPULAR, ACCOUNT_SEARCH, ACCOUNT_TRENDS,
               ACCOUNT_CATEGORY, ACCOUNT_CHANNEL, ACCOUNT_FAVORITES, ACCOUNT_CHECK, ACCOUNT_VIDEOS,
               ACCOUNT_CLIPS } AccountAction;

typedef struct {
    Thread thread;
    bool running, success;
    atomic_bool cancelled, finished, code_ready;
    AccountAction action;
    TwitchAccount account;
    ChannelList rows;
    ChannelProfile profile;
    int page_step;
    unsigned route_revision, from_page;
    char cursor[CATALOG_CURSOR_BYTES];
    char query[FAVORITE_QUERY_BYTES], code[20], activation_link[128], status[160];
} AccountJob;

bool account_start(AccountJob *, AccountAction, const TwitchAccount *, const char *query, const char *cursor);
void account_join(AccountJob *);
void account_clear(TwitchAccount *);
bool account_due(const TwitchAccount *);

#endif
