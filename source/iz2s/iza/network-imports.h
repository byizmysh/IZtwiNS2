/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef IZTWINS2_NETWORK_IMPORTS_H
#define IZTWINS2_NETWORK_IMPORTS_H
/* Socket calls are provided by the engine's own standard libounce client.
 * Host-backed thread and keyboard calls use the launcher's public table. */
#define IZTWINS2_HOST_IMPORTS(X) \
    X(threadCreate) \
    X(threadStart) \
    X(threadWaitForExit) \
    X(threadClose) \
    X(swkbdCreate) \
    X(swkbdClose) \
    X(swkbdConfigMakePresetDefault) \
    X(swkbdConfigSetGuideText) \
    X(swkbdConfigSetInitialText) \
    X(swkbdShow)
#endif
