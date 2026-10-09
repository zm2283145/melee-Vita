/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Vita API subset on PS5: see shim/psp2_shim.h. */
#include "psp2_shim.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ps5_log.h"
#include "ps5_services.h"

/* ---- time ---------------------------------------------------------------- */

SceUInt64 sceKernelGetProcessTimeWide(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (SceUInt64) now.tv_sec * 1000000u + (SceUInt64) now.tv_nsec / 1000u;
}

SceUInt64 sceKernelGetSystemTimeWide(void) { return sceKernelGetProcessTimeWide(); }

int sceKernelExitProcess(int result)
{
    melee_ps5_log("process exit: %d", result);
    melee_ps5_log_flush();
    _Exit(result);
}

/* The Vita platform layer treats a PlayStation TV as "no touch screen, maybe
 * a DualShock": the closest description of a PS5. */
int sceKernelIsPSVitaTV(void) { return 1; }
int sceKernelGetProcessId(void) { return (int) getpid(); }

/* ---- threads ------------------------------------------------------------- */

#define SHIM_MAX_THREADS 64
#define SHIM_THREAD_UID_BASE 0x4000

typedef struct ShimThread {
    bool used;
    bool started;
    bool finished;
    pthread_t thread;
    SceKernelThreadEntry entry;
    size_t stack_size;
    char name[32];
    void* args;
    SceSize arglen;
    int exit_status;
} ShimThread;

static ShimThread s_threads[SHIM_MAX_THREADS];
static pthread_mutex_t s_thread_lock = PTHREAD_MUTEX_INITIALIZER;
/* The calling thread's UID (no compiler TLS: the decomp is built with GCC,
 * which has no emulated-TLS mode for this target). */
static pthread_key_t s_uid_key;
static pthread_once_t s_uid_once = PTHREAD_ONCE_INIT;
static void uid_key_create(void) { pthread_key_create(&s_uid_key, NULL); }
static void set_current_uid(int uid)
{
    pthread_once(&s_uid_once, uid_key_create);
    pthread_setspecific(s_uid_key, (void*) (intptr_t) uid);
}
static int current_uid(void)
{
    pthread_once(&s_uid_once, uid_key_create);
    return (int) (intptr_t) pthread_getspecific(s_uid_key);
}

static ShimThread* thread_from_uid(SceUID uid)
{
    const int index = uid - SHIM_THREAD_UID_BASE;
    if (index < 0 || index >= SHIM_MAX_THREADS || !s_threads[index].used) return NULL;
    return &s_threads[index];
}

SceUID sceKernelCreateThread(const char* name, SceKernelThreadEntry entry,
                             int priority, SceSize stack_size, SceUInt attr,
                             int cpu_affinity_mask, const void* option)
{
    (void) priority; (void) attr; (void) cpu_affinity_mask; (void) option;
    pthread_mutex_lock(&s_thread_lock);
    for (int i = 0; i < SHIM_MAX_THREADS; ++i) {
        if (s_threads[i].used) continue;
        memset(&s_threads[i], 0, sizeof(s_threads[i]));
        s_threads[i].used = true;
        s_threads[i].entry = entry;
        /* x86-64 frames are larger than ARM ones: give every thread room. */
        s_threads[i].stack_size = stack_size < 0x40000u ? 0x40000u : stack_size * 2u;
        snprintf(s_threads[i].name, sizeof(s_threads[i].name), "%s", name ? name : "thread");
        pthread_mutex_unlock(&s_thread_lock);
        return SHIM_THREAD_UID_BASE + i;
    }
    pthread_mutex_unlock(&s_thread_lock);
    melee_ps5_log("sceKernelCreateThread(%s): no free slot", name ? name : "?");
    return -1;
}

static void* thread_trampoline(void* opaque)
{
    ShimThread* t = opaque;
    set_current_uid(SHIM_THREAD_UID_BASE + (int) (t - s_threads));
    t->exit_status = t->entry(t->arglen, t->args);
    t->finished = true;
    return NULL;
}

int sceKernelStartThread(SceUID thid, SceSize arglen, void* argp)
{
    ShimThread* t = thread_from_uid(thid);
    pthread_attr_t attr;
    int result;
    if (t == NULL || t->started) return -1;
    /* Like the Vita kernel, give the thread its own copy of the arguments. */
    if (arglen != 0u && argp != NULL) {
        t->args = malloc(arglen);
        if (t->args == NULL) return -1;
        memcpy(t->args, argp, arglen);
        t->arglen = arglen;
    }
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, t->stack_size);
    result = pthread_create(&t->thread, &attr, thread_trampoline, t);
    pthread_attr_destroy(&attr);
    if (result != 0) {
        melee_ps5_log("pthread_create(%s) failed: %d", t->name, result);
        return -1;
    }
    t->started = true;
    return 0;
}

int sceKernelWaitThreadEnd(SceUID thid, int* stat, SceUInt* timeout)
{
    ShimThread* t = thread_from_uid(thid);
    if (t == NULL || !t->started) return -1;
    if (timeout != NULL) {
        const SceUInt64 deadline = sceKernelGetProcessTimeWide() + *timeout;
        while (!t->finished) {
            if (sceKernelGetProcessTimeWide() >= deadline) return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            usleep(1000);
        }
    }
    pthread_join(t->thread, NULL);
    t->started = false;
    if (stat != NULL) *stat = t->exit_status;
    return 0;
}

int sceKernelDeleteThread(SceUID thid)
{
    ShimThread* t = thread_from_uid(thid);
    if (t == NULL) return -1;
    if (t->started && !t->finished) return -1;
    if (t->started) pthread_detach(t->thread);
    free(t->args);
    pthread_mutex_lock(&s_thread_lock);
    memset(t, 0, sizeof(*t));
    pthread_mutex_unlock(&s_thread_lock);
    return 0;
}

int sceKernelExitThread(int status)
{
    ShimThread* t = thread_from_uid(current_uid());
    if (t != NULL) {
        t->exit_status = status;
        t->finished = true;
    }
    pthread_exit(NULL);
}

int sceKernelExitDeleteThread(int status)
{
    ShimThread* t = thread_from_uid(current_uid());
    if (t != NULL) {
        /* Nobody joins a self-deleting thread. */
        pthread_detach(pthread_self());
        free(t->args);
        pthread_mutex_lock(&s_thread_lock);
        memset(t, 0, sizeof(*t));
        pthread_mutex_unlock(&s_thread_lock);
    }
    (void) status;
    pthread_exit(NULL);
}

int sceKernelGetThreadId(void)
{
    const int uid = current_uid();
    return uid != 0 ? uid : 1;
}

int sceKernelDelayThread(SceUInt delay_us)
{
    if (delay_us == 0u) {
        sched_yield();
        return 0;
    }
    usleep(delay_us);
    return 0;
}

int sceKernelDelayThreadCB(SceUInt delay_us) { return sceKernelDelayThread(delay_us); }
int sceKernelChangeThreadCpuAffinityMask(SceUID thid, int mask) { (void) thid; (void) mask; return 0; }
int sceKernelChangeThreadPriority(SceUID thid, int priority) { (void) thid; (void) priority; return 0; }

/* ---- semaphores ---------------------------------------------------------- */

#define SHIM_MAX_SEMAS 64
#define SHIM_SEMA_UID_BASE 0x5000

typedef struct ShimSema {
    bool used;
    int count;
    int max;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} ShimSema;

static ShimSema s_semas[SHIM_MAX_SEMAS];
static pthread_mutex_t s_sema_table_lock = PTHREAD_MUTEX_INITIALIZER;

static ShimSema* sema_from_uid(SceUID uid)
{
    const int index = uid - SHIM_SEMA_UID_BASE;
    if (index < 0 || index >= SHIM_MAX_SEMAS || !s_semas[index].used) return NULL;
    return &s_semas[index];
}

SceUID sceKernelCreateSema(const char* name, SceUInt attr, int init_val,
                           int max_val, const void* option)
{
    (void) attr; (void) option;
    pthread_mutex_lock(&s_sema_table_lock);
    for (int i = 0; i < SHIM_MAX_SEMAS; ++i) {
        if (s_semas[i].used) continue;
        s_semas[i].used = true;
        s_semas[i].count = init_val;
        s_semas[i].max = max_val;
        pthread_mutex_init(&s_semas[i].lock, NULL);
        pthread_cond_init(&s_semas[i].cond, NULL);
        pthread_mutex_unlock(&s_sema_table_lock);
        return SHIM_SEMA_UID_BASE + i;
    }
    pthread_mutex_unlock(&s_sema_table_lock);
    melee_ps5_log("sceKernelCreateSema(%s): no free slot", name ? name : "?");
    return -1;
}

int sceKernelDeleteSema(SceUID semaid)
{
    ShimSema* s = sema_from_uid(semaid);
    if (s == NULL) return -1;
    pthread_mutex_lock(&s_sema_table_lock);
    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->lock);
    s->used = false;
    pthread_mutex_unlock(&s_sema_table_lock);
    return 0;
}

int sceKernelSignalSema(SceUID semaid, int signal)
{
    ShimSema* s = sema_from_uid(semaid);
    if (s == NULL) return -1;
    pthread_mutex_lock(&s->lock);
    s->count += signal;
    if (s->max > 0 && s->count > s->max) s->count = s->max;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

static void deadline_after(struct timespec* ts, SceUInt us)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += us / 1000000u;
    ts->tv_nsec += (long) (us % 1000000u) * 1000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

int sceKernelWaitSema(SceUID semaid, int signal, SceUInt* timeout)
{
    ShimSema* s = sema_from_uid(semaid);
    struct timespec deadline;
    if (s == NULL) return -1;
    if (timeout != NULL) deadline_after(&deadline, *timeout);
    pthread_mutex_lock(&s->lock);
    while (s->count < signal) {
        if (timeout != NULL) {
            if (pthread_cond_timedwait(&s->cond, &s->lock, &deadline) == ETIMEDOUT &&
                s->count < signal) {
                pthread_mutex_unlock(&s->lock);
                return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            }
        } else {
            pthread_cond_wait(&s->cond, &s->lock);
        }
    }
    s->count -= signal;
    pthread_mutex_unlock(&s->lock);
    return 0;
}

int sceKernelPollSema(SceUID semaid, int signal)
{
    ShimSema* s = sema_from_uid(semaid);
    int result = -1;
    if (s == NULL) return -1;
    pthread_mutex_lock(&s->lock);
    if (s->count >= signal) {
        s->count -= signal;
        result = 0;
    }
    pthread_mutex_unlock(&s->lock);
    return result;
}

/* ---- mutexes ------------------------------------------------------------- */

#define SHIM_MAX_MUTEXES 64
#define SHIM_MUTEX_UID_BASE 0x6000

typedef struct ShimMutex {
    bool used;
    pthread_mutex_t lock;
} ShimMutex;

static ShimMutex s_mutexes[SHIM_MAX_MUTEXES];

static void init_recursive(pthread_mutex_t* mutex)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

SceUID sceKernelCreateMutex(const char* name, SceUInt attr, int init_count,
                            const void* option)
{
    (void) name; (void) attr; (void) option;
    pthread_mutex_lock(&s_sema_table_lock);
    for (int i = 0; i < SHIM_MAX_MUTEXES; ++i) {
        if (s_mutexes[i].used) continue;
        s_mutexes[i].used = true;
        init_recursive(&s_mutexes[i].lock);
        pthread_mutex_unlock(&s_sema_table_lock);
        /* The Vita layer creates mutexes with an initial count of 1 and never
         * releases that count; on Vita the creator's first unlock does.  Start
         * unlocked, which is the state those callers rely on. */
        (void) init_count;
        return SHIM_MUTEX_UID_BASE + i;
    }
    pthread_mutex_unlock(&s_sema_table_lock);
    return -1;
}

static ShimMutex* mutex_from_uid(SceUID uid)
{
    const int index = uid - SHIM_MUTEX_UID_BASE;
    if (index < 0 || index >= SHIM_MAX_MUTEXES || !s_mutexes[index].used) return NULL;
    return &s_mutexes[index];
}

int sceKernelDeleteMutex(SceUID mutexid)
{
    ShimMutex* m = mutex_from_uid(mutexid);
    if (m == NULL) return -1;
    pthread_mutex_destroy(&m->lock);
    m->used = false;
    return 0;
}

int sceKernelLockMutex(SceUID mutexid, int lock_count, SceUInt* timeout)
{
    ShimMutex* m = mutex_from_uid(mutexid);
    (void) timeout;
    if (m == NULL) return -1;
    for (int n = 0; n < lock_count; ++n) pthread_mutex_lock(&m->lock);
    return 0;
}

int sceKernelTryLockMutex(SceUID mutexid, int lock_count)
{
    ShimMutex* m = mutex_from_uid(mutexid);
    if (m == NULL) return -1;
    for (int n = 0; n < lock_count; ++n) {
        if (pthread_mutex_trylock(&m->lock) != 0) {
            while (n-- > 0) pthread_mutex_unlock(&m->lock);
            return -1;
        }
    }
    return 0;
}

int sceKernelUnlockMutex(SceUID mutexid, int unlock_count)
{
    ShimMutex* m = mutex_from_uid(mutexid);
    if (m == NULL) return -1;
    for (int n = 0; n < unlock_count; ++n) pthread_mutex_unlock(&m->lock);
    return 0;
}

/* A lightweight mutex keeps its pthread mutex behind a pointer in the work
 * area, so the work struct's size never depends on pthread_mutex_t. */
_Static_assert(sizeof(SceKernelLwMutexWork) >= sizeof(void*), "lw mutex work");

int sceKernelCreateLwMutex(SceKernelLwMutexWork* work, const char* name,
                           unsigned int attr, int init_count, const void* option)
{
    pthread_mutex_t* mutex = malloc(sizeof(*mutex));
    (void) name; (void) attr; (void) option;
    if (mutex == NULL) return -1;
    init_recursive(mutex);
    memset(work, 0, sizeof(*work));
    memcpy(work, &mutex, sizeof(mutex));
    for (int n = 0; n < init_count; ++n) pthread_mutex_lock(mutex);
    return 0;
}

static pthread_mutex_t* lw_mutex(SceKernelLwMutexWork* work)
{
    pthread_mutex_t* mutex;
    memcpy(&mutex, work, sizeof(mutex));
    return mutex;
}

int sceKernelDeleteLwMutex(SceKernelLwMutexWork* work)
{
    pthread_mutex_t* mutex = lw_mutex(work);
    if (mutex == NULL) return -1;
    pthread_mutex_destroy(mutex);
    free(mutex);
    memset(work, 0, sizeof(*work));
    return 0;
}

int sceKernelLockLwMutex(SceKernelLwMutexWork* work, int lock_count, unsigned int* timeout)
{
    pthread_mutex_t* mutex = lw_mutex(work);
    (void) timeout;
    if (mutex == NULL) return -1;
    for (int n = 0; n < lock_count; ++n) pthread_mutex_lock(mutex);
    return 0;
}

int sceKernelTryLockLwMutex(SceKernelLwMutexWork* work, int lock_count)
{
    pthread_mutex_t* mutex = lw_mutex(work);
    if (mutex == NULL) return -1;
    for (int n = 0; n < lock_count; ++n) {
        if (pthread_mutex_trylock(mutex) != 0) {
            while (n-- > 0) pthread_mutex_unlock(mutex);
            return -1;
        }
    }
    return 0;
}

int sceKernelUnlockLwMutex(SceKernelLwMutexWork* work, int unlock_count)
{
    pthread_mutex_t* mutex = lw_mutex(work);
    if (mutex == NULL) return -1;
    for (int n = 0; n < unlock_count; ++n) pthread_mutex_unlock(mutex);
    return 0;
}

/* ---- memory blocks: plain aligned heap memory ----------------------------- */

#define SHIM_MAX_BLOCKS 64
#define SHIM_BLOCK_UID_BASE 0x7000
static void* s_blocks[SHIM_MAX_BLOCKS];

SceUID sceKernelAllocMemBlock(const char* name, SceKernelMemBlockType type,
                              SceSize size, const void* opt)
{
    (void) name; (void) type; (void) opt;
    for (int i = 0; i < SHIM_MAX_BLOCKS; ++i) {
        if (s_blocks[i] != NULL) continue;
        s_blocks[i] = aligned_alloc(4096, (size + 4095u) & ~4095u);
        return s_blocks[i] != NULL ? SHIM_BLOCK_UID_BASE + i : -1;
    }
    return -1;
}

int sceKernelFreeMemBlock(SceUID uid)
{
    const int index = uid - SHIM_BLOCK_UID_BASE;
    if (index < 0 || index >= SHIM_MAX_BLOCKS || s_blocks[index] == NULL) return -1;
    free(s_blocks[index]);
    s_blocks[index] = NULL;
    return 0;
}

int sceKernelGetMemBlockBase(SceUID uid, void** base)
{
    const int index = uid - SHIM_BLOCK_UID_BASE;
    if (index < 0 || index >= SHIM_MAX_BLOCKS || s_blocks[index] == NULL) return -1;
    *base = s_blocks[index];
    return 0;
}

int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo* info)
{
    memset(info, 0, sizeof(*info));
    info->size_user = 256 * 1024 * 1024;
    info->size_cdram = 256 * 1024 * 1024;
    info->size = info->size_user;
    return 0;
}

/* ---- io ------------------------------------------------------------------ */

const char* melee_ps5_translate_path(const char* path, char* out, size_t size)
{
    /* Titles are sandboxed: the disc image ships inside the title folder
     * (/app0) and everything the game writes goes to /download0. */
    static const struct { const char* from; const char* to; } map[] = {
        { "ux0:data/melee/GALE01.iso", "/app0/GALE01.iso" },
        { "ux0:data/melee/", "/download0/" },
        { "ux0:data/melee", "/download0" },
        { "ux0:data/", "/download0/" },
        { "ux0:/data/", "/download0/" },
        { "ux0:", "/download0/" },
        { "ur0:", "/download0/" },
        { "app0:/", "/app0/" },
        { "app0:", "/app0/" },
    };
    if (path == NULL) return NULL;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        const size_t n = strlen(map[i].from);
        if (strncmp(path, map[i].from, n) == 0) {
            snprintf(out, size, "%s%s", map[i].to, path + n);
            return out;
        }
    }
    snprintf(out, size, "%s", path);
    return out;
}

static int posix_flags(int flags)
{
    int result;
    switch (flags & SCE_O_RDWR) {
    case SCE_O_WRONLY: result = O_WRONLY; break;
    case SCE_O_RDWR: result = O_RDWR; break;
    default: result = O_RDONLY; break;
    }
    if (flags & SCE_O_APPEND) result |= O_APPEND;
    if (flags & SCE_O_CREAT) result |= O_CREAT;
    if (flags & SCE_O_TRUNC) result |= O_TRUNC;
    if (flags & SCE_O_EXCL) result |= O_EXCL;
    return result;
}

/* Vita file descriptors are positive UIDs; keep PS5 fds above 0 too. */
SceUID sceIoOpen(const char* file, int flags, SceMode mode)
{
    char path[512];
    int fd = open(melee_ps5_translate_path(file, path, sizeof(path)),
                  posix_flags(flags), mode ? mode : 0666);
    return fd >= 0 ? fd : (SceUID) (0x80010000 | (errno & 0xffff));
}

int sceIoClose(SceUID fd) { return close(fd); }

SceSSize sceIoRead(SceUID fd, void* data, SceSize size)
{
    const ssize_t n = read(fd, data, size);
    return n >= 0 ? (SceSSize) n : -1;
}

SceSSize sceIoWrite(SceUID fd, const void* data, SceSize size)
{
    const ssize_t n = write(fd, data, size);
    return n >= 0 ? (SceSSize) n : -1;
}

SceSSize sceIoPread(SceUID fd, void* data, SceSize size, SceOff offset)
{
    const ssize_t n = pread(fd, data, size, offset);
    return n >= 0 ? (SceSSize) n : -1;
}

SceSSize sceIoPwrite(SceUID fd, const void* data, SceSize size, SceOff offset)
{
    const ssize_t n = pwrite(fd, data, size, offset);
    return n >= 0 ? (SceSSize) n : -1;
}

SceOff sceIoLseek(SceUID fd, SceOff offset, int whence)
{
    return lseek(fd, offset, whence);
}

int sceIoMkdir(const char* dir, SceMode mode)
{
    char path[512];
    return mkdir(melee_ps5_translate_path(dir, path, sizeof(path)), mode ? mode : 0777) == 0
        ? 0 : -1;
}

int sceIoRmdir(const char* dir)
{
    char path[512];
    return rmdir(melee_ps5_translate_path(dir, path, sizeof(path))) == 0 ? 0 : -1;
}

int sceIoRemove(const char* file)
{
    char path[512];
    return unlink(melee_ps5_translate_path(file, path, sizeof(path))) == 0 ? 0 : -1;
}

int sceIoRename(const char* oldname, const char* newname)
{
    char a[512], b[512];
    return rename(melee_ps5_translate_path(oldname, a, sizeof(a)),
                  melee_ps5_translate_path(newname, b, sizeof(b))) == 0 ? 0 : -1;
}

int sceIoGetstat(const char* file, SceIoStat* stat_out)
{
    char path[512];
    struct stat st;
    if (stat(melee_ps5_translate_path(file, path, sizeof(path)), &st) != 0) return -1;
    memset(stat_out, 0, sizeof(*stat_out));
    stat_out->st_mode = S_ISDIR(st.st_mode) ? SCE_S_IFDIR : SCE_S_IFREG;
    stat_out->st_size = st.st_size;
    return 0;
}

/* ---- audio: Vita ports at any rate, resampled to the PS5's 48 kHz -------- */

#define SHIM_MAX_PORTS 4

typedef struct ShimAudioPort {
    bool used;
    int handle;
    int in_frames;
    int out_frames;
    int rate;
    int channels;
    double phase;
    int16_t last[2];
    int16_t* out;
    SceUInt64 next_due;
} ShimAudioPort;

static ShimAudioPort s_ports[SHIM_MAX_PORTS];

int sceAudioOutOpenPort(SceAudioOutPortType type, int len, int freq,
                        SceAudioOutMode mode)
{
    (void) type;
    for (int i = 0; i < SHIM_MAX_PORTS; ++i) {
        ShimAudioPort* p = &s_ports[i];
        if (p->used) continue;
        p->in_frames = len;
        p->rate = freq;
        p->channels = mode == SCE_AUDIO_OUT_MODE_MONO ? 1 : 2;
        /* PS5 buffers are multiples of 256 frames; round the 48 kHz grain. */
        p->out_frames = (int) (((int64_t) len * 48000 / freq + 255) / 256 * 256);
        p->out = calloc((size_t) p->out_frames * 2u, sizeof(int16_t));
        if (p->out == NULL) return -1;
        p->handle = melee_ps5_audio_open((uint32_t) p->out_frames);
        if (p->handle < 0) {
            free(p->out);
            return p->handle;
        }
        p->used = true;
        return i;
    }
    return -1;
}

int sceAudioOutReleasePort(int port)
{
    if (port < 0 || port >= SHIM_MAX_PORTS || !s_ports[port].used) return -1;
    melee_ps5_audio_close(s_ports[port].handle);
    free(s_ports[port].out);
    memset(&s_ports[port], 0, sizeof(s_ports[port]));
    return 0;
}

/* Linear resampling.  The output grain is fixed, so the input consumed per
 * call is fractional; the remainder carries over in `phase`.  Vita audio
 * code hands us exactly in_frames per call, which slightly under-feeds the
 * fixed 48 kHz grain when it was rounded up; the ring buffer upstream
 * absorbs that. */
int sceAudioOutOutput(int port, const void* buf)
{
    ShimAudioPort* p;
    const int16_t* in = buf;
    double step;
    if (port < 0 || port >= SHIM_MAX_PORTS || !s_ports[port].used) return -1;
    p = &s_ports[port];
    if (buf == NULL) return 0;
    step = (double) p->in_frames / (double) p->out_frames;
    for (int i = 0; i < p->out_frames; ++i) {
        const double pos = p->phase + i * step;
        const int index = (int) pos;
        const double frac = pos - index;
        for (int c = 0; c < 2; ++c) {
            const int src_c = p->channels == 1 ? 0 : c;
            const int16_t a = index == 0 ? p->last[c] : in[(index - 1) * p->channels + src_c];
            const int16_t b = index < p->in_frames ? in[index * p->channels + src_c] : a;
            p->out[i * 2 + c] = (int16_t) (a + (b - a) * frac);
        }
    }
    p->phase = 0.0;
    p->last[0] = in[(p->in_frames - 1) * p->channels];
    p->last[1] = in[(p->in_frames - 1) * p->channels + (p->channels == 1 ? 0 : 1)];
    {
        /* sceAudioOutOutput blocks until the grain plays on hardware; if it
         * returns early (an error, or a non-blocking port) pace the caller to
         * real time so it cannot drain the mixer's ring in a busy loop. */
        const SceUInt64 grain_us = (SceUInt64) p->out_frames * 1000000u / 48000u;
        const int result = melee_ps5_audio_output(p->handle, p->out);
        const SceUInt64 now = sceKernelGetProcessTimeWide();
        static unsigned logged;
        if (logged < 8u) {
            ++logged;
            melee_ps5_log("[AUDIO] sceAudioOutOutput(%d) = 0x%08x", p->handle, (unsigned) result);
        }
        if (p->next_due == 0u || now > p->next_due + grain_us * 4u) p->next_due = now;
        p->next_due += grain_us;
        if (p->next_due > now + 2000u) usleep((useconds_t) (p->next_due - now - 1000u));
        return result;
    }
}

/* ---- controller ------------------------------------------------------------ */

int sceCtrlSetSamplingMode(int mode) { (void) mode; return 0; }
int sceCtrlSetSamplingModeExt(int mode) { (void) mode; return 0; }

static int ctrl_peek(int port, SceCtrlData* data, int count)
{
    MeleePs5PadState state;
    if (data == NULL || count < 1) return -1;
    memset(data, 0, sizeof(*data) * (size_t) count);
    data->lx = data->ly = data->rx = data->ry = 128;
    data->timeStamp = sceKernelGetProcessTimeWide();
    /* Vita port 0 is the handheld; PS TV ports 1..4 are external pads. */
    if (!melee_ps5_pad_read(port <= 1 ? 0 : port - 1, &state)) return 1;
    data->buttons = state.buttons;
    data->lx = state.lx;
    data->ly = state.ly;
    data->rx = state.rx;
    data->ry = state.ry;
    data->lt = state.l2;
    data->rt = state.r2;
    return 1;
}

int sceCtrlPeekBufferPositive(int port, SceCtrlData* d, int c) { return ctrl_peek(port, d, c); }
int sceCtrlPeekBufferPositive2(int port, SceCtrlData* d, int c) { return ctrl_peek(port, d, c); }
int sceCtrlPeekBufferPositiveExt2(int port, SceCtrlData* d, int c) { return ctrl_peek(port, d, c); }
int sceCtrlReadBufferPositive(int port, SceCtrlData* d, int c) { return ctrl_peek(port, d, c); }

/* ---- touch, display, modules ---------------------------------------------- */

int sceTouchSetSamplingState(int port, int state) { (void) port; (void) state; return 0; }

int sceTouchGetPanelInfo(int port, SceTouchPanelInfo* info)
{
    (void) port;
    memset(info, 0, sizeof(*info));
    info->maxDispX = 1919;
    info->maxDispY = 1087;
    return 0;
}

int sceTouchPeek(int port, SceTouchData* data, int count)
{
    (void) port;
    if (data == NULL || count < 1) return -1;
    memset(data, 0, sizeof(*data));
    return 1;
}

/* Presentation waits for vblank in eglSwapBuffers; VI does not wait again. */
int sceDisplayWaitVblankStart(void) { return 0; }

int sceSysmoduleLoadModule(int id) { (void) id; return 0; }
int sceSysmoduleUnloadModule(int id) { (void) id; return 0; }

/* ---- libc file calls with Vita paths (shim/ps5_compat.h) ------------------- */

FILE* melee_ps5_fopen(const char* path, const char* mode)
{
    char buffer[512];
    return fopen(melee_ps5_translate_path(path, buffer, sizeof(buffer)), mode);
}

int melee_ps5_remove(const char* path)
{
    char buffer[512];
    return remove(melee_ps5_translate_path(path, buffer, sizeof(buffer)));
}

int melee_ps5_rename(const char* from, const char* to)
{
    char a[512], b[512];
    return rename(melee_ps5_translate_path(from, a, sizeof(a)),
                  melee_ps5_translate_path(to, b, sizeof(b)));
}

int melee_ps5_mkdir(const char* path, mode_t mode)
{
    char buffer[512];
    return mkdir(melee_ps5_translate_path(path, buffer, sizeof(buffer)), mode);
}

DIR* melee_ps5_opendir(const char* path)
{
    char buffer[512];
    return opendir(melee_ps5_translate_path(path, buffer, sizeof(buffer)));
}

int melee_ps5_stat(const char* path, struct stat* st)
{
    char buffer[512];
    return stat(melee_ps5_translate_path(path, buffer, sizeof(buffer)), st);
}

/* ---- libc gaps ------------------------------------------------------------- */

#include <time.h>

/* The PS5 libc exports localtime but not localtime_r. */
struct tm* localtime_r(const time_t* clock, struct tm* result)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    struct tm* value;
    pthread_mutex_lock(&lock);
    value = localtime(clock);
    if (value != NULL) *result = *value;
    pthread_mutex_unlock(&lock);
    return value != NULL ? result : NULL;
}

/* Some shared sources declare this one directly instead of through the
 * shim header; libkernel has no function of this name to collide with. */
#undef sceKernelGetProcessTimeWide
SceUInt64 sceKernelGetProcessTimeWide(void) { return melee_psp2_sceKernelGetProcessTimeWide(); }
