// SPDX-License-Identifier: GPL-3.0-only
// A native app requests shell replacement and waits. Returning through the
// payload C runtime's exit path issues a forbidden process syscall (SIGSYS).
// If the accepted asynchronous request stalls, make one exact-title self-close
// request after three seconds; never target or discover an arbitrary title.
#include "platform.h"
#include <stdint.h>
#include <string.h>
int sceKernelUsleep(uint32_t microseconds);
int sceSystemServiceGetStatus(void* status);
int sceLncUtilInitialize(void);
int sceLncUtilGetAppIdOfRunningBigApp(void);
int sceLncUtilGetAppTitleId(uint32_t app_id, char* title_id);
int sceLncUtilKillApp(uint32_t app_id);
__attribute__((used)) const char gs_exact_self_close_marker[] =
    "ISSHELF_EXACT_SELF_CLOSE_V1";
int gs_app_visibility(unsigned* flags) {
    if (!flags)
        return -1;
    /* Public SystemService status ABI (also documented by shadPS4): event count
     * followed by byte-sized overlay/background flags. A generously sized
     * output buffer retains reserved fields without importing an SDK struct. */
    _Alignas(16) unsigned char status[512];
    memset(status, 0, sizeof(status));
    int result = sceSystemServiceGetStatus(status);
    if (result)
        return result;
    if (status[4] > 1 || status[5] > 1)
        return -2;
    *flags = (unsigned)status[4] | ((unsigned)status[5] << 1);
    return 0;
}
int gs_request_close(void) {
    return sceSystemServiceLoadExec("exit", 0);
}
void gs_wait_for_close(void) {
    for (unsigned i = 0; i < 30; ++i)
        sceKernelUsleep(100000);
    const int initialized = sceLncUtilInitialize();
    const uint32_t app_id = initialized == 0 ?
        (uint32_t)sceLncUtilGetAppIdOfRunningBigApp() : UINT32_MAX;
    char title[16] = {0};
    if (initialized == 0 && app_id != 0 && app_id != UINT32_MAX &&
        sceLncUtilGetAppTitleId(app_id, title) == 0 &&
        memchr(title, 0, sizeof(title)) != 0 &&
        strcmp(title, "PPSA99541") == 0)
        (void)sceLncUtilKillApp(app_id);
    for (;;)
        sceKernelUsleep(100000);
}
