/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gx_submit.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdlib.h>
#include <string.h>

#include "../vita_log.h"
#include "profiler_live.h"

#ifndef MELEE_VITA_NO_SUBMIT_THREAD

/* Small enough to stay in L2: records are written by one core and read
 * by another, and a large ring made every record a cache miss. */
#define GXS_RING_SIZE (1024u * 1024u) /* power of two */
#define GXS_ALIGN 8u
#define GXS_CPU_MASK_USER_2 0x40000

typedef struct GxsHeader {
    GxsRun run; /* NULL: padding up to the end of the ring */
    u32 size;   /* whole record, header included */
} GxsHeader;

/* From the libc heap: a static ring grew the ELF image enough that the 48 MB
 * GPU geometry arena no longer fit in the process memory budget. */
static u8* s_ring;
/* Monotonic byte positions; offset = position & (size - 1). */
static u32 s_write;           /* game thread only */
static volatile u32 s_commit; /* published by the game thread */
static volatile u32 s_read;   /* advanced by the worker */
static volatile int s_waiting;
static SceUID s_sema = -1;
static SceUID s_thread = -1;
static SceUID s_game_thread = -1;
static int s_running;

static inline u32 load_acquire(volatile u32* p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static int gxs_worker(SceSize args, void* argp)
{
    (void) args;
    (void) argp;
    for (;;) {
        u32 commit = __atomic_load_n(&s_commit, __ATOMIC_SEQ_CST);
        u32 read = s_read;
        if (read == commit) {
            __atomic_store_n(&s_waiting, 1, __ATOMIC_SEQ_CST);
            commit = __atomic_load_n(&s_commit, __ATOMIC_SEQ_CST);
            if (read == commit) {
                sceKernelWaitSema(s_sema, 1, NULL);
                continue;
            }
            __atomic_store_n(&s_waiting, 0, __ATOMIC_SEQ_CST);
        }
        while (read != commit) {
            GxsHeader* header =
                (GxsHeader*) (s_ring + (read & (GXS_RING_SIZE - 1u)));
            if (header->run != NULL) header->run(header + 1);
            read += header->size;
            __atomic_store_n(&s_read, read, __ATOMIC_RELEASE);
        }
    }
    return 0;
}

void gxs_init(void)
{
    if (s_running) return;
    s_game_thread = sceKernelGetThreadId();
    s_ring = aligned_alloc(64, GXS_RING_SIZE);
    if (s_ring == NULL) {
        melee_vita_log_info("[GXS] ring allocation failed");
        return;
    }
    s_sema = sceKernelCreateSema("melee gx submit", 0, 0, 0x7fffffff, NULL);
    if (s_sema < 0) {
        melee_vita_log_info("[GXS] semaphore failed 0x%08x", (unsigned) s_sema);
        return;
    }
    /* Runtime shader compilation happens here too, so give it a big stack.
     * Priority: above the skinning worker, below the audio output thread. */
    s_thread = sceKernelCreateThread("melee gx submit", gxs_worker,
                                     0x10000100 - 15, 1024 * 1024, 0,
                                     GXS_CPU_MASK_USER_2, NULL);
    if (s_thread < 0 || sceKernelStartThread(s_thread, 0, NULL) < 0) {
        melee_vita_log_info("[GXS] worker thread failed 0x%08x",
                            (unsigned) s_thread);
        s_thread = -1;
        return;
    }
    s_running = 1;
    melee_vita_log_info("[GXS] submit worker started on core 2");
}

bool gxs_active(void) { return s_running != 0; }

void gxs_publish(void)
{
    if (!s_running || s_commit == s_write) return;
    __atomic_store_n(&s_commit, s_write, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_waiting, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&s_waiting, 0, __ATOMIC_SEQ_CST);
        sceKernelSignalSema(s_sema, 1);
    }
}

static void wait_for_space(u32 bytes)
{
    /* The worker is a full ring of records behind: let it run. */
    while (GXS_RING_SIZE - (s_write - load_acquire(&s_read)) < bytes) {
        gxs_publish();
    }
}

void* gxs_alloc(GxsRun run, u32 payload_size)
{
    u32 need, offset;
    GxsHeader* header;
    if (!s_running) return NULL;
    gxs_publish();
    need = (u32) ((sizeof(GxsHeader) + payload_size + GXS_ALIGN - 1u) &
                  ~(GXS_ALIGN - 1u));
    if (need > GXS_RING_SIZE / 4u) return NULL;
    offset = s_write & (GXS_RING_SIZE - 1u);
    if (offset + need > GXS_RING_SIZE) {
        const u32 pad = GXS_RING_SIZE - offset;
        wait_for_space(pad);
        header = (GxsHeader*) (s_ring + offset);
        header->run = NULL;
        header->size = pad;
        s_write += pad;
        offset = 0;
    }
    wait_for_space(need);
    header = (GxsHeader*) (s_ring + offset);
    header->run = run;
    header->size = need;
    s_write += need;
    return header + 1;
}

void gxs_drain(void)
{
    if (!s_running) return;
    if (sceKernelGetThreadId() != s_game_thread) return;
    gxs_publish();
    if (load_acquire(&s_read) == s_write) return;
    {
#ifdef MELEE_VITA_PROFILER
        const u64 started = sceKernelGetProcessTimeWide();
#endif
        while (load_acquire(&s_read) != s_write) {
            /* Spin: the game thread has nothing else to do until the
             * worker catches up, and a sleep would cost a scheduler tick. */
        }
#ifdef MELEE_VITA_PROFILER
        melee_vita_profiler_record_duration(58u, 1u);
        melee_vita_profiler_record_duration(
            59u, sceKernelGetProcessTimeWide() - started);
#endif
    }
}

#else

void gxs_init(void) {}
bool gxs_active(void) { return false; }
void* gxs_alloc(GxsRun run, u32 payload_size)
{
    (void) run;
    (void) payload_size;
    return NULL;
}
void gxs_publish(void) {}
void gxs_drain(void) {}

#endif
