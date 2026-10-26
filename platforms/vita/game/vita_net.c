/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shared SceNet start/stop.  The updater and Slippi Direct play can both be
 * online at the same time.  SceNet only accepts one sceNetInit, and the first
 * sceNetTerm would close the other one's sockets, so both count their use
 * here instead of starting SceNet themselves. */
#include "vita_net.h"

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* The updater used a 1 MiB SceNet pool before this file existed, so keep it. */
#define VITA_NET_MEMORY_SIZE (1024 * 1024)

/* vita-headers has no name for this one.  newlib's vitanet.c defines the
 * same value: sceNetCtlInit returns it when SceNetCtl is already running. */
#ifndef SCE_NET_CTL_ERROR_NOT_TERMINATED
#define SCE_NET_CTL_ERROR_NOT_TERMINATED 0x80412102
#endif

static pthread_mutex_t s_net_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_users;
static void* s_net_memory;

/* What this file started itself, so melee_vita_net_stop only undoes that.
 * Debug builds start SceNet in vita_log.c before main() and never stop it;
 * then sceNetInit says EBUSY here and we just borrow it. */
static int s_module_loaded;
static int s_net_owned;
static int s_ctl_owned;

static void net_cleanup(void)
{
    if (s_ctl_owned) {
        sceNetCtlTerm();
        s_ctl_owned = 0;
    }
    if (s_net_owned) {
        sceNetTerm();
        s_net_owned = 0;
    }
    if (s_net_memory != NULL) {
        free(s_net_memory);
        s_net_memory = NULL;
    }
    if (s_module_loaded) {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
        s_module_loaded = 0;
    }
}

int melee_vita_net_start(void)
{
    SceNetInitParam init;
    int result;

    pthread_mutex_lock(&s_net_lock);

    /* Somebody already started it, just count one more user. */
    if (s_users > 0) {
        s_users++;
        pthread_mutex_unlock(&s_net_lock);
        return 0;
    }

    if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED) {
        result = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
        if (result < 0) {
            net_cleanup();
            pthread_mutex_unlock(&s_net_lock);
            return result;
        }
        s_module_loaded = 1;
    }

    s_net_memory = malloc(VITA_NET_MEMORY_SIZE);
    if (s_net_memory == NULL) {
        net_cleanup();
        pthread_mutex_unlock(&s_net_lock);
        return (int) SCE_NET_ERROR_ENOMEM;
    }

    memset(&init, 0, sizeof(init));
    init.memory = s_net_memory;
    init.size = VITA_NET_MEMORY_SIZE;
    init.flags = 0;
    result = sceNetInit(&init);
    if ((unsigned int) result == SCE_NET_ERROR_EBUSY) {
        /* Already running (debug logger).  SceNet keeps using the other
         * pool, so ours is not needed. */
        free(s_net_memory);
        s_net_memory = NULL;
    } else if (result < 0) {
        net_cleanup();
        pthread_mutex_unlock(&s_net_lock);
        return result;
    } else {
        s_net_owned = 1;
    }

    result = sceNetCtlInit();
    if ((unsigned int) result == SCE_NET_CTL_ERROR_NOT_TERMINATED) {
        /* Same as above: someone else started SceNetCtl first. */
    } else if (result < 0) {
        net_cleanup();
        pthread_mutex_unlock(&s_net_lock);
        return result;
    } else {
        s_ctl_owned = 1;
    }

    s_users = 1;
    pthread_mutex_unlock(&s_net_lock);
    return 0;
}

void melee_vita_net_stop(void)
{
    pthread_mutex_lock(&s_net_lock);
    if (s_users > 0) {
        s_users--;
        if (s_users == 0) net_cleanup();
    }
    pthread_mutex_unlock(&s_net_lock);
}
