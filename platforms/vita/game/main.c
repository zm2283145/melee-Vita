/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vita_platform.h"
#include "../vita_log.h"

#include <psp2/kernel/processmgr.h>

#include <stdio.h>

/* Reserve enough libc heap for MEM1 now and ARAM plus backend allocations in
 * later milestones. GXM allocations remain outside this heap. */
unsigned int _newlib_heap_size_user = 128u * 1024u * 1024u;
/* The runtime shader compiler (libshacccg) recurses deeply; the default
 * 256 KiB main-thread stack is not enough for it. */
int sceUserMainThreadStackSize = 4 * 1024 * 1024;

int melee_main(void);

int main(void)
{
    int log_result = melee_vita_log_start();
    int result;
    printf("Melee Vita: logger startup %s\n", log_result == 0 ? "passed" : "failed");
    melee_vita_log_info("Melee Vita full boot: process entered");

    result = melee_vita_platform_init();
    if (result < 0) {
        melee_vita_log_info("Platform initialization failed: 0x%08X",
                            (unsigned int) result);
        printf("Melee Vita: platform initialization failed: 0x%08X\n",
               (unsigned int) result);
        melee_vita_log_stop();
        sceKernelExitProcess(result);
        return result;
    }

#ifdef MELEE_VITA_WAIT_FOR_DEBUGGER
    if (melee_vita_debugger_wait() < 0) {
        melee_vita_log_info("[DEBUGGER] failed to enter GDB wait");
        printf("Melee Vita: debugger startup failed\n");
    }
#endif

    melee_vita_log_info("Platform initialized; entering melee_main");
    result = melee_main();
    melee_vita_log_info("melee_main returned: %d", result);
    melee_vita_platform_shutdown();
    melee_vita_log_info("Platform shutdown complete");
    melee_vita_log_stop();
    sceKernelExitProcess(result);
    return result;
}
