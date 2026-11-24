/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef IZTWINS2_NATIVE_STORAGE_H
#define IZTWINS2_NATIVE_STORAGE_H
#include <stdbool.h>
#include <stdio.h>
#include <sb4/native-app.h>

bool iztwi_storage_bind(IzNativeApp *app);
FILE *iztwi_storage_read(const char *path);
bool iztwi_storage_publish(const char *path, const char *data, size_t size);
bool iztwi_storage_remove(const char *path);
void iztwi_storage_note(const char *path, const char *data, size_t size);
#endif
