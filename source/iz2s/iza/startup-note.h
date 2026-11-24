/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef IZTWINS2_STARTUP_NOTE_H
#define IZTWINS2_STARTUP_NOTE_H

#include <sb4/native-app.h>

static inline void startup_note(IzNativeApp *app, const char *file, const char *stage, uint32_t code) {
    if (!app || !app->write_file) return;
    char line[96];
    unsigned size = 0;
    while (*stage && size < sizeof(line) - 12) line[size++] = *stage++;
    line[size++] = ' ';
    line[size++] = '0';
    line[size++] = 'x';
    for (unsigned bit = 32; bit; bit -= 4) {
        unsigned digit = (code >> (bit - 4)) & 15;
        line[size++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
    }
    line[size++] = '\n';
    app->write_file(app, file, line, size);
}

#endif
