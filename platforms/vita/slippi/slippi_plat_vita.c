/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Vita platform layer for the Slippi network client (slippi_plat.h).  The
 * 3DS port has the same functions on libctru in slippi_plat.c; everything
 * else in this folder is shared with it unchanged. */
#include "slippi_plat.h"

#include "../game/vita_net.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* user.json and config.ini live here.  The port keeps everything else under
 * ux0:data/melee too (dvd.c), so this is just one more folder. */
#define SLIPPI_DEFAULT_DATA_DIR "ux0:data/melee/slippi"

/* -------------------------------------------------------------- time */

uint64_t sp_time_us(void)
{
    /* Microseconds since the process started.  Never goes backwards. */
    return (uint64_t) sceKernelGetProcessTimeWide();
}

void sp_sleep_us(uint32_t us)
{
    sceKernelDelayThread(us);
}

static uint32_t s_random_state;

uint32_t sp_random(void)
{
    uint32_t x;

    /* Seed from the clock the first time.  The client only uses this for
     * the local port and Dolphin's rng_offset, so it doesn't need to be a
     * good random number, just different every launch. */
    if (s_random_state == 0) {
        uint64_t now = sp_time_us();
        s_random_state = (uint32_t) (now ^ (now >> 32)) | 1u;
    }

    /* xorshift32, same as the 3DS build. */
    x = s_random_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_random_state = x;
    return x;
}

/* -------------------------------------------------------------- data dir */

static char s_data_dir[160];

void sp_set_data_dir(const char* dir)
{
    if (dir == NULL) dir = "";
    snprintf(s_data_dir, sizeof(s_data_dir), "%s", dir);
}

const char* sp_data_dir(void)
{
    if (s_data_dir[0] != '\0') return s_data_dir;
    return SLIPPI_DEFAULT_DATA_DIR;
}

char* sp_read_file(const char* path, size_t* length)
{
    FILE* file;
    long size;
    size_t got;
    char* buffer;

    file = fopen(path, "rb");
    if (file == NULL) return NULL;

    fseek(file, 0, SEEK_END);
    size = ftell(file);
    rewind(file);

    /* user.json and config.ini are tiny.  Anything over 1 MiB is not ours. */
    if (size < 0 || size > 1024 * 1024) {
        fclose(file);
        return NULL;
    }

    buffer = malloc((size_t) size + 1);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    got = fread(buffer, 1, (size_t) size, file);
    fclose(file);
    buffer[got] = '\0';
    if (length != NULL) *length = got;
    return buffer;
}

/* -------------------------------------------------------------- lock */

/* The client keeps all its state behind one lock (see slippi_net.h), taken by
 * the game thread and the network thread. */
static pthread_mutex_t s_state_lock = PTHREAD_MUTEX_INITIALIZER;

void sp_lock_init(void)
{
    /* Nothing to do, PTHREAD_MUTEX_INITIALIZER already set it up. */
}

void sp_lock(void)
{
    pthread_mutex_lock(&s_state_lock);
}

void sp_unlock(void)
{
    pthread_mutex_unlock(&s_state_lock);
}

/* -------------------------------------------------------------- log */

/* Release builds compile vita_log.c out, so the client writes its own log
 * file next to user.json, like game.log on the 3DS.  It is the first thing
 * to ask for when someone reports a connection problem. */
static pthread_mutex_t s_log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE* s_log_file;
static int s_log_failed;
static void (*s_log_sink)(const char* line);

void sp_set_log_sink(void (*sink)(const char* line))
{
    s_log_sink = sink;
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* Reads up to three digits as one IPv4 number.  Returns -1 if it isn't one. */
static int read_ip_number(const char** text)
{
    int value = 0;
    int digits = 0;

    while (is_digit(**text) && digits < 3) {
        value = value * 10 + (**text - '0');
        (*text)++;
        digits++;
    }
    if (digits == 0 || value > 255) return -1;
    return value;
}

static int is_private_ip(const int ip[4])
{
    if (ip[0] == 10) return 1;                                   /* LAN */
    if (ip[0] == 192 && ip[1] == 168) return 1;                  /* LAN */
    if (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) return 1;    /* LAN */
    if (ip[0] == 169 && ip[1] == 254) return 1;                  /* link-local */
    if (ip[0] == 127) return 1;                                  /* loopback */
    if (ip[0] == 0) return 1;
    if (ip[0] == 1 && ip[1] == 1 && ip[2] == 1 && ip[3] == 1) return 1;
    return 0;
}

/* People post this log in public bug reports, so public IPv4 addresses (the
 * opponent's home connection) become "public-ip".  LAN addresses stay,
 * because they show whether it was a LAN or an internet match.  The 3DS
 * port does the same in its slippi_plat.c. */
static void hide_public_ips(char* line, size_t size)
{
    char out[256];
    size_t used = 0;
    const char* read = line;

    while (*read != '\0' && used + 1 < sizeof(out)) {
        int starts_number = is_digit(*read);
        int ip[4];
        int ok = 1;
        int i;
        const char* end = read;

        /* Only look at numbers that don't continue an earlier number. */
        if (read != line && (is_digit(read[-1]) || read[-1] == '.')) starts_number = 0;

        if (!starts_number) {
            out[used++] = *read++;
            continue;
        }

        for (i = 0; i < 4 && ok; i++) {
            ip[i] = read_ip_number(&end);
            if (ip[i] < 0) {
                ok = 0;
            } else if (i < 3) {
                if (*end == '.') end++;
                else ok = 0;
            }
        }
        /* "1.2.3.4.5" or "1.2.3.45678" is not an address. */
        if (ok && is_digit(*end)) ok = 0;
        if (ok && *end == '.' && is_digit(end[1])) ok = 0;

        if (!ok) {
            out[used++] = *read++;
            continue;
        }

        if (is_private_ip(ip)) {
            while (read < end && used + 1 < sizeof(out)) out[used++] = *read++;
        } else {
            const char* hidden = "public-ip";
            while (*hidden != '\0' && used + 1 < sizeof(out)) out[used++] = *hidden++;
            read = end;
        }
    }

    out[used] = '\0';
    snprintf(line, size, "%s", out);
}

static void open_log_file(void)
{
    char path[256];

    if (s_log_file != NULL || s_log_failed) return;

    /* mkdir fails if the folder already exists, that's fine. */
    mkdir("ux0:data/melee", 0777);
    mkdir(sp_data_dir(), 0777);

    snprintf(path, sizeof(path), "%s/slippi.log", sp_data_dir());
    s_log_file = fopen(path, "w");
    if (s_log_file == NULL) s_log_failed = 1;
}

void sp_log(const char* fmt, ...)
{
    char line[256];
    va_list args;
    uint64_t now;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    hide_public_ips(line, sizeof(line));

    pthread_mutex_lock(&s_log_lock);
    if (s_log_sink != NULL) {
        s_log_sink(line);
    } else {
        open_log_file();
        if (s_log_file != NULL) {
            now = sp_time_us();
            fprintf(s_log_file, "[%7u.%03u] %s\n", (unsigned int) (now / 1000000u),
                    (unsigned int) ((now / 1000u) % 1000u), line);
            /* Flush every line so the log survives a crash or a hard power
             * off, which is exactly when it is needed. */
            fflush(s_log_file);
        }
    }
    pthread_mutex_unlock(&s_log_lock);
}

void sp_log_drain(void)
{
    /* The 3DS queues network-thread lines here because its log isn't
     * thread-safe.  sp_log above writes straight away under its own lock,
     * so there is nothing to drain. */
}

/* -------------------------------------------------------------- thread */

static SceUID s_thread = -1;
static SceUID s_thread_id = -1;
static void (*s_thread_fn)(void*);
static void* s_thread_arg;

static int thread_entry(SceSize args, void* argp)
{
    (void) args;
    (void) argp;
    s_thread_id = sceKernelGetThreadId();
    s_thread_fn(s_thread_arg);
    return 0;
}

int sp_thread_start(void (*fn)(void*), void* arg, int core)
{
    int cpu_mask;
    int result;

    if (s_thread >= 0) return -1;

    /* The 3DS passes -2 for "default".  Core 0 runs the game and core 1 the
     * renderer (gxm_game.c), so the network thread goes on core 2 with
     * audio, DVD and the GX submit worker.  It sleeps 2 ms between passes,
     * so it doesn't take much from them. */
    if (core == 0) cpu_mask = SCE_KERNEL_CPU_MASK_USER_0;
    else if (core == 1) cpu_mask = SCE_KERNEL_CPU_MASK_USER_1;
    else cpu_mask = SCE_KERNEL_CPU_MASK_USER_2;

    s_thread_fn = fn;
    s_thread_arg = arg;
    s_thread_id = -1;

    /* Priority: above the game thread (0x10000100) so ENet answers pings
     * while a frame runs, below audio (-20) and GX submit (-15).  The DNS
     * lookup runs on this thread, so give it a roomy stack. */
    s_thread = sceKernelCreateThread("slippi net", thread_entry, 0x10000100 - 10,
                                     256 * 1024, 0, cpu_mask, NULL);
    if (s_thread < 0) {
        sp_log("network thread: create failed 0x%08x", (unsigned int) s_thread);
        s_thread = -1;
        return -1;
    }

    result = sceKernelStartThread(s_thread, 0, NULL);
    if (result < 0) {
        sp_log("network thread: start failed 0x%08x", (unsigned int) result);
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
        return -1;
    }
    return 0;
}

void sp_thread_join(void)
{
    if (s_thread < 0) return;

    /* slippi_net.c sets its stop flag before calling this, so the thread
     * leaves its loop within one 2 ms sleep. */
    sceKernelWaitThreadEnd(s_thread, NULL, NULL);
    sceKernelDeleteThread(s_thread);
    s_thread = -1;
    s_thread_id = -1;
}

int sp_on_thread(void)
{
    if (s_thread < 0) return 0;
    return sceKernelGetThreadId() == s_thread_id;
}

/* -------------------------------------------------------------- network */

static int s_net_started;

int sp_net_init(void)
{
    int result;

    if (s_net_started) return 0;

    result = melee_vita_net_start();
    if (result < 0) {
        sp_log("network start failed: 0x%08x", (unsigned int) result);
        return -1;
    }
    s_net_started = 1;
    return 0;
}

void sp_net_exit(void)
{
    if (!s_net_started) return;
    melee_vita_net_stop();
    s_net_started = 0;
}

uint32_t sp_lan_ip_fallback(void)
{
    SceNetCtlInfo info;
    SceNetInAddr address;

    if (!s_net_started) return 0;

    memset(&info, 0, sizeof(info));
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0) return 0;

    /* Same as gethostid() on the 3DS: network byte order, 0 if unknown. */
    memset(&address, 0, sizeof(address));
    if (sceNetInetPton(SCE_NET_AF_INET, info.ip_address, &address) != 1) return 0;
    return address.s_addr;
}

int sp_wifi_status(void)
{
    int state = SCE_NETCTL_STATE_DISCONNECTED;

    if (!s_net_started) return -1;
    if (sceNetCtlInetGetState(&state) < 0) return -1;
    if (state == SCE_NETCTL_STATE_CONNECTED) return 1;
    return 0;
}

int sp_app_running(void)
{
    /* The 3DS asks aptMainLoop() here.  The Vita closes the app for us from
     * the LiveArea, so there is nothing to check. */
    return 1;
}
