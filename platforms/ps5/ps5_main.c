/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PS5 entry point: the counterpart of platforms/vita/game/main.c. */
#include "vita_platform.h"
#include "../vita/vita_log.h"
#include "ps5_log.h"
#include "ps5_services.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int melee_main(void);

#define GAME_STACK_SIZE (16u * 1024u * 1024u)

static void on_fatal_signal(int sig)
{
    melee_ps5_log("fatal signal %d", sig);
    melee_ps5_log_flush();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void* game_thread(void* arg)
{
    int result;
    (void) arg;
    result = melee_vita_platform_init();
    if (result < 0) {
        melee_ps5_log("platform initialisation failed: %d", result);
        melee_ps5_notify("Melee: graphics initialisation failed");
        return (void*) (intptr_t) result;
    }
    melee_ps5_log("platform initialised; entering melee_main");
    result = melee_main();
    melee_ps5_log("melee_main returned %d", result);
    melee_vita_platform_shutdown();
    return (void*) (intptr_t) result;
}

int main(void)
{
    pthread_attr_t attr;
    pthread_t thread;
    void* result = NULL;
    struct stat st;

    melee_vita_log_start();
    melee_ps5_log("Melee PS5 starting");
    melee_ps5_notify("Melee PS5: starting");
    signal(SIGSEGV, on_fatal_signal);
    signal(SIGBUS, on_fatal_signal);
    signal(SIGILL, on_fatal_signal);
    signal(SIGFPE, on_fatal_signal);
    signal(SIGABRT, on_fatal_signal);

    if (melee_ps5_services_init() != 0) melee_ps5_log("services init incomplete");
    if (stat("/app0/GALE01.iso", &st) != 0) {
        melee_ps5_log("disc image missing: /app0/GALE01.iso");
        melee_ps5_notify("Melee: put GALE01.iso in the title folder");
        for (;;) sleep(60);
    }
    melee_ps5_log("disc image: %lld bytes", (long long) st.st_size);

    /* The game's deepest call chains want more than the main thread has. */
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, GAME_STACK_SIZE);
    if (pthread_create(&thread, &attr, game_thread, NULL) != 0) {
        melee_ps5_log("could not start the game thread");
        for (;;) sleep(60);
    }
    pthread_attr_destroy(&attr);
    pthread_join(thread, &result);
    melee_ps5_log("game thread ended: %d", (int) (intptr_t) result);
    melee_ps5_log_flush();
    /* Returning from main reads as a crash to the shell; stay resident until
     * the user closes the title. */
    for (;;) sleep(60);
}
