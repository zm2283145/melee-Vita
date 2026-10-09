/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PS5 entry point: the counterpart of platforms/vita/game/main.c. */
#include "vita_platform.h"
#include "../vita/vita_log.h"
#include "ps5_log.h"
#include "ps5_services.h"

#include <pthread.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int melee_main(void);

#define GAME_STACK_SIZE (16u * 1024u * 1024u)

#include <ucontext.h>

int main(void);

/* The driver's malloc heap (native-app/app_heap.c) defaults to 128 MiB; the
 * shader compiler ran it dry mid-match (operator new[] -> ud2).  Above 128 MiB
 * it comes from CPU-cached direct memory. */
const size_t ps5_opengl_heap_size = 1024u * 1024u * 1024u;

/* libkernel: fills a module-info record; the name is at offset 8 in every
 * layout, so a generous buffer with a plausible size field is enough. */
int sceKernelGetModuleInfoFromAddr(const void* addr, int flags, void* info);

static void describe_address(const char* label, uintptr_t value)
{
    static uint64_t info[0x400 / 8];
    const uintptr_t self = (uintptr_t) main;
    if (value + 0x800000u >= self && value < self + 0x2000000u) {
        /* Symbolise as (link address of main in llvm-pie.elf) + this. */
        melee_ps5_log("%s 0x%llx  main%+lld", label, (unsigned long long) value,
                      (long long) (value - self));
        return;
    }
    memset(info, 0, sizeof(info));
    info[0] = 0x160;
    if (sceKernelGetModuleInfoFromAddr((const void*) value, 1, info) != 0) {
        memset(info, 0, sizeof(info));
        info[0] = 0x1a8;
        if (sceKernelGetModuleInfoFromAddr((const void*) value, 1, info) != 0) {
            if (label[0] != ' ') melee_ps5_log("%s 0x%llx  (no module)", label, (unsigned long long) value);
            return;
        }
    }
    ((char*) info)[8 + 255] = 0;
    {
        /* ModuleInfo (0x160): segments start at 264 as {addr, size, prot}. */
        const uintptr_t seg0 = (uintptr_t) info[33];
        melee_ps5_log("%s 0x%llx  %s (+0x%llx)", label, (unsigned long long) value,
                      (const char*) info + 8, (unsigned long long) (value - seg0));
    }
}

static void on_fatal_signal(int sig, siginfo_t* info, void* context)
{
    const ucontext_t* uc = context;
    extern char __executable_start[] __attribute__((weak));
    const uintptr_t rip = uc != NULL ? (uintptr_t) uc->uc_mcontext.mc_rip : 0u;
    const uintptr_t rsp = uc != NULL ? (uintptr_t) uc->uc_mcontext.mc_rsp : 0u;
    melee_ps5_log("FATAL signal %d addr=%p rip=%p rsp=%p main=%p",
                  sig, info != NULL ? info->si_addr : NULL, (void*) rip, (void*) rsp,
                  (void*) main);
    if (uc != NULL) {
        /* The PS5's ucontext layout is not FreeBSD's: dump it raw, marking
         * words that point into the executable. */
        const uintptr_t* words = (const uintptr_t*) uc;
        const uintptr_t base = (uintptr_t) main;
        for (int i = 0; i < 96; ++i) {
            const uintptr_t v = words[i];
            const bool code = v + 0x800000u >= base && v < base + 0x2000000u;
            if (code || (i < 40 && v != 0))
                melee_ps5_log("  uc[%d] = 0x%llx%s", i, (unsigned long long) v, code ? "  <code>" : "");
        }
    }
    if (uc != NULL) {
        /* Walk the words around every stack-looking register value, naming
         * the module of each address that falls inside one. */
        const uintptr_t* words = (const uintptr_t*) uc;
        uintptr_t seen[8];
        int nseen = 0;
        describe_address("rip", (uintptr_t) uc->uc_mcontext.mc_rip);
        for (int i = 2; i < 40; ++i) {
            const uintptr_t v = words[i] & ~(uintptr_t) 7u;
            bool dup = false;
            if (v < 0x7e0000000ull || v >= 0x800000000ull) continue; /* thread stacks */
            for (int j = 0; j < nseen; ++j) dup |= seen[j] == v;
            if (dup || nseen == 8) continue;
            seen[nseen++] = v;
            melee_ps5_log("  stack from uc[%d]=0x%llx:", i, (unsigned long long) v);
            for (int k = -4; k < 48; ++k) {
                char label[32];
                const uintptr_t w = ((const uintptr_t*) v)[k];
                if (w < 0x400000u || w >= 0x1000000000ull) continue;
                snprintf(label, sizeof(label), "   [%+d]", k);
                describe_address(label, w);
            }
        }
    }
    melee_ps5_log_flush();
    melee_ps5_log_drain();
    (void) __executable_start;
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
    /* The GL runtime prints its per-phase draw profile to stdout. */
    if (freopen("/download0/gl-stdout.log", "w", stdout) != NULL) setvbuf(stdout, NULL, _IOLBF, 0);
    melee_ps5_log("Melee PS5 starting (main at %p; subtract main's link address for offsets)",
                  (void*) main);
    melee_ps5_notify("Melee PS5: starting");
    {
        static const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_sigaction = on_fatal_signal;
        action.sa_flags = SA_SIGINFO;
        for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i)
            sigaction(signals[i], &action, NULL);
    }

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
