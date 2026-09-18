#include "vita_log.h"

#ifndef MELEE_VITA_RELEASE
#include <uvdb.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef MELEE_VITA_LOG_HOST
#define MELEE_VITA_LOG_HOST "10.1.1.146"
#endif

#ifndef MELEE_VITA_DEBUGNET_PORT
#define MELEE_VITA_DEBUGNET_PORT 18194
#endif

static unsigned char s_net_memory[1024 * 1024] __attribute__((aligned(64)));
static int s_module_loaded;
static int s_net_started;
static int s_netctl_started;
static int s_logger_started;

int melee_vita_log_start(void)
{
    if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) goto failure;
    s_module_loaded = 1;
    SceNetInitParam init = {
        .memory = s_net_memory,
        .size = sizeof(s_net_memory),
        .flags = 0,
    };
    if (sceNetInit(&init) < 0) goto failure;
    s_net_started = 1;
    if (sceNetCtlInit() < 0) goto failure;
    s_netctl_started = 1;
    const struct uvdb_debugnet_config config = {
        .server_ip = MELEE_VITA_LOG_HOST,
        .port = MELEE_VITA_DEBUGNET_PORT,
        .level = UVDB_LOG_DEBUG,
    };
    if (uvdb_debugnet_start(&config) < 0) goto failure;
    s_logger_started = 1;
    return 0;

failure:
    melee_vita_log_stop();
    return -1;
}

void melee_vita_log_info(const char* format, ...)
{
    if (!s_logger_started) return;
    char message[768];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    uvdb_debugnet_write(UVDB_LOG_INFO, message);
}

int melee_vita_debugger_wait(void)
{
    const struct uvdb_config config = {
        .port = 1234,
        .max_packet_buffer = 256u * 1024u,
    };

    if (!s_net_started || uvdb_configure(&config) < 0) return -1;

    melee_vita_log_info("[DEBUGGER] waiting for GDB on TCP port 1234");
    uvdb_enter();

    if (uvdb_get_state() == UVDB_STATE_ERROR) return -1;
    melee_vita_log_info("[DEBUGGER] GDB resumed the game");
    return 0;
}

void melee_vita_log_stop(void)
{
    uvdb_shutdown();
    if (s_logger_started) uvdb_debugnet_stop();
    if (s_netctl_started) sceNetCtlTerm();
    if (s_net_started) sceNetTerm();
    if (s_module_loaded) sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    s_logger_started = s_netctl_started = s_net_started = s_module_loaded = 0;
}
#endif
