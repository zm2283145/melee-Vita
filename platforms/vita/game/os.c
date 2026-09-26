/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Vita implementation of the Dolphin OS foundation used during game boot. */
#include "vita_platform.h"
#include "opening_audio.h"
#include "gxm_game.h"
#include "../vita_log.h"

#include <dolphin/os.h>
#include <dolphin/os/OSAlarm.h>
#include <dolphin/os/OSError.h>

#include <psp2/kernel/processmgr.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MELEE_GCN_UNIX_EPOCH 946684800LL
#define MELEE_ARENA_OFFSET 0x3100u

uintptr_t OSBaseAddress;

static void* s_mem1;
static u8* s_arena_lo;
static u8* s_arena_hi;
static OSAlarm* s_alarms;
static OSTime s_time_origin;
static int s_interrupt_depth;
static BOOL s_progressive;
static BOOL s_eurgb60;
static u32 s_sound_mode = OS_SOUND_MODE_STEREO;

static OSTime usec_to_ticks(uint64_t usec)
{
    return (OSTime) ((usec * (uint64_t) OS_TIMER_CLOCK) / 1000000u);
}

static uintptr_t align_up(uintptr_t value, u32 alignment)
{
    return alignment > 1 ? (value + alignment - 1u) & ~(uintptr_t) (alignment - 1u)
                         : value;
}

static uintptr_t align_down(uintptr_t value, u32 alignment)
{
    return alignment > 1 ? value & ~(uintptr_t) (alignment - 1u) : value;
}

#ifdef MELEE_VITA_HANG_WATCHDOG
#include <psp2/kernel/threadmgr.h>
extern u32 g_melee_vita_vi_calls;

/* Debug aid: if the game stops presenting frames for a long time, fault on
 * purpose so the Vita writes a core dump holding the main thread's stack. */
static int hang_watchdog_thread(SceSize args, void* argp)
{
    u32 last = 0;
    int stalled_seconds = 0;
    (void) args;
    (void) argp;
    for (;;) {
        sceKernelDelayThread(1000 * 1000);
        if (g_melee_vita_vi_calls != last) {
            last = g_melee_vita_vi_calls;
            stalled_seconds = 0;
            continue;
        }
        if (last == 0) continue;
        if (++stalled_seconds == MELEE_VITA_HANG_WATCHDOG) {
            melee_vita_log_info("[WATCHDOG] no frame for %d s (vi_calls=%u); forcing core dump",
                                stalled_seconds, (unsigned) last);
            sceKernelDelayThread(500 * 1000);
            {
                static volatile uintptr_t bad_address = 0x10u;
                *(u32*) bad_address = 0xDEADu;
            }
        }
    }
    return 0;
}
#endif

int melee_vita_platform_init(void)
{
#ifdef MELEE_VITA_HANG_WATCHDOG
    SceUID watchdog = sceKernelCreateThread("melee_watchdog", hang_watchdog_thread,
                                            0x40, 0x4000, 0, 0, NULL);
    if (watchdog >= 0) sceKernelStartThread(watchdog, 0, NULL);
#endif
    return melee_vita_gxm_init();
}

void melee_vita_platform_poll(void)
{
    /* DVD callbacks may enqueue ARQ copies, so preserve this order. */
    melee_vita_dvd_poll();
    melee_vita_audio_poll();
    melee_vita_card_poll();
    melee_vita_opening_audio_poll();
}

void melee_vita_platform_shutdown(void)
{
    melee_vita_dvd_shutdown();
    melee_vita_opening_audio_shutdown();
    melee_vita_gxm_shutdown();
    melee_vita_audio_shutdown();
    free(s_mem1);
    s_mem1 = NULL;
    OSBaseAddress = 0;
}

void OSInit(void)
{
    OSBootInfo* boot;
    if (s_mem1 != NULL) return;

    s_mem1 = aligned_alloc(32, MELEE_VITA_MEM1_SIZE);
    if (s_mem1 == NULL) {
        OSPanic(__FILE__, __LINE__, "unable to allocate %u-byte MEM1 arena",
                MELEE_VITA_MEM1_SIZE);
    }
    memset(s_mem1, 0, MELEE_VITA_MEM1_SIZE);
    OSBaseAddress = (uintptr_t) s_mem1;
    s_arena_lo = (u8*) s_mem1 + MELEE_ARENA_OFFSET;
    s_arena_hi = (u8*) s_mem1 + MELEE_VITA_MEM1_SIZE;

    boot = (OSBootInfo*) s_mem1;
    boot->memorySize = MELEE_VITA_MEM1_SIZE;
    boot->arenaLo = s_arena_lo;
    boot->arenaHi = s_arena_hi;
    __OSBusClock = OS_BUS_CLOCK;
    __OSCoreClock = OS_CORE_CLOCK;

    s_time_origin = OSGetSystemTime() -
                    usec_to_ticks(sceKernelGetProcessTimeWide());
}

void* OSGetArenaHi(void) { return s_arena_hi; }
void* OSGetArenaLo(void) { return s_arena_lo; }

void OSSetArenaHi(void* value)
{
    if ((u8*) value < (u8*) s_mem1 ||
        (u8*) value > (u8*) s_mem1 + MELEE_VITA_MEM1_SIZE)
        OSPanic(__FILE__, __LINE__, "arena high pointer outside MEM1");
    s_arena_hi = value;
}

void OSSetArenaLo(void* value)
{
    if ((u8*) value < (u8*) s_mem1 ||
        (u8*) value > (u8*) s_mem1 + MELEE_VITA_MEM1_SIZE)
        OSPanic(__FILE__, __LINE__, "arena low pointer outside MEM1");
    s_arena_lo = value;
}

void* OSAllocFromArenaLo(u32 size, u32 alignment)
{
    u8* result = (u8*) align_up((uintptr_t) s_arena_lo, alignment);
    u8* next = (u8*) align_up((uintptr_t) result + size, alignment);
    if (next > s_arena_hi) return NULL;
    s_arena_lo = next;
    return result;
}

void* OSAllocFromArenaHi(u32 size, u32 alignment)
{
    u8* result = (u8*) align_down((uintptr_t) s_arena_hi - size, alignment);
    if (result < s_arena_lo) return NULL;
    s_arena_hi = result;
    return result;
}

u32 OSGetPhysicalMemSize(void) { return MELEE_VITA_MEM1_SIZE; }
u32 OSGetConsoleSimulatedMemSize(void) { return MELEE_VITA_MEM1_SIZE; }
u32 OSGetConsoleType(void) { return OS_CONSOLE_RETAIL4; }

void* OSPhysicalToCached(u32 address)
{
    return (void*) (OSBaseAddress + address);
}

void* OSPhysicalToUncached(u32 address) { return OSPhysicalToCached(address); }

u32 OSCachedToPhysical(void* address)
{
    return (u32) ((uintptr_t) address - OSBaseAddress);
}

u32 OSUncachedToPhysical(void* address) { return OSCachedToPhysical(address); }
void* OSCachedToUncached(void* address) { return address; }
void* OSUncachedToCached(void* address) { return address; }

OSTime OSGetSystemTime(void)
{
    time_t now = time(NULL);
    return (OSTime) (now - MELEE_GCN_UNIX_EPOCH) * OS_TIMER_CLOCK;
}

OSTime OSGetNativeTime(void)
{
    return usec_to_ticks(sceKernelGetProcessTimeWide());
}

OSTime OSGetTime(void) { return s_time_origin + OSGetNativeTime(); }
OSTick OSGetTick(void) { return (OSTick) OSGetTime(); }

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* out)
{
    time_t seconds = (time_t) (ticks / OS_TIMER_CLOCK + MELEE_GCN_UNIX_EPOCH);
    struct tm value;
    localtime_r(&seconds, &value);
    out->sec = value.tm_sec;
    out->min = value.tm_min;
    out->hour = value.tm_hour;
    out->mday = value.tm_mday;
    out->mon = value.tm_mon;
    out->year = value.tm_year + 1900;
    out->wday = value.tm_wday;
    out->yday = value.tm_yday;
    ticks %= OS_TIMER_CLOCK;
    if (ticks < 0) ticks += OS_TIMER_CLOCK;
    out->msec = (int) (ticks * 1000 / OS_TIMER_CLOCK);
    out->usec = (int) (ticks * 1000000 / OS_TIMER_CLOCK) - out->msec * 1000;
}

OSTime OSCalendarTimeToTicks(OSCalendarTime* value)
{
    struct tm input;
    memset(&input, 0, sizeof(input));
    input.tm_sec = value->sec;
    input.tm_min = value->min;
    input.tm_hour = value->hour;
    input.tm_mday = value->mday;
    input.tm_mon = value->mon;
    input.tm_year = value->year - 1900;
    input.tm_isdst = -1;
    return (OSTime) (mktime(&input) - MELEE_GCN_UNIX_EPOCH) * OS_TIMER_CLOCK +
           OSMillisecondsToTicks(value->msec) +
           OSMicrosecondsToTicks(value->usec);
}

BOOL OSDisableInterrupts(void)
{
    BOOL enabled = s_interrupt_depth == 0;
    ++s_interrupt_depth;
    return enabled;
}

BOOL OSEnableInterrupts(void)
{
    BOOL enabled = s_interrupt_depth == 0;
    s_interrupt_depth = 0;
    melee_vita_os_run_alarms();
    return enabled;
}

BOOL OSRestoreInterrupts(BOOL enabled)
{
    BOOL was_enabled = s_interrupt_depth == 0;
    if (enabled) {
        s_interrupt_depth = 0;
        melee_vita_os_run_alarms();
    } else if (s_interrupt_depth > 0) {
        --s_interrupt_depth;
    }
    return was_enabled;
}

void OSInitAlarm(void) {}

void OSCreateAlarm(OSAlarm* alarm)
{
    memset(alarm, 0, sizeof(*alarm));
}

static void insert_alarm(OSAlarm* alarm, OSTime fire, OSAlarmHandler handler)
{
    BOOL enabled = OSDisableInterrupts();
    if (alarm->handler != NULL) OSCancelAlarm(alarm);
    alarm->handler = handler;
    alarm->fire = fire;
    alarm->prev = NULL;
    alarm->next = s_alarms;
    if (s_alarms != NULL) s_alarms->prev = alarm;
    s_alarms = alarm;
    OSRestoreInterrupts(enabled);
}

void OSSetAlarm(OSAlarm* alarm, OSTime ticks, OSAlarmHandler handler)
{
    alarm->period = 0;
    insert_alarm(alarm, OSGetTime() + ticks, handler);
}

void OSSetAbsAlarm(OSAlarm* alarm, OSTime time, OSAlarmHandler handler)
{
    alarm->period = 0;
    insert_alarm(alarm, time, handler);
}

void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period,
                        OSAlarmHandler handler)
{
    alarm->period = period;
    alarm->start = start;
    insert_alarm(alarm, start, handler);
}

void OSCancelAlarm(OSAlarm* alarm)
{
    BOOL enabled = OSDisableInterrupts();
    if (alarm->handler != NULL) {
        if (alarm->prev != NULL) alarm->prev->next = alarm->next;
        else if (s_alarms == alarm) s_alarms = alarm->next;
        if (alarm->next != NULL) alarm->next->prev = alarm->prev;
        alarm->handler = NULL;
        alarm->prev = alarm->next = NULL;
    }
    OSRestoreInterrupts(enabled);
}

void OSSetAlarmTag(OSAlarm* alarm, u32 tag) { alarm->tag = tag; }

void OSCancelAlarms(u32 tag)
{
    OSAlarm* alarm = s_alarms;
    while (alarm != NULL) {
        OSAlarm* next = alarm->next;
        if (alarm->tag == tag) OSCancelAlarm(alarm);
        alarm = next;
    }
}

BOOL OSCheckAlarmQueue(void) { return s_alarms != NULL; }

void melee_vita_os_run_alarms(void)
{
    static int delivering;
    OSAlarm* alarm;
    OSTime now;
    if (delivering || s_interrupt_depth != 0) return;
    delivering = 1;
    now = OSGetTime();
    alarm = s_alarms;
    while (alarm != NULL) {
        OSAlarm* next = alarm->next;
        if (alarm->handler != NULL && alarm->fire <= now) {
            OSAlarmHandler handler = alarm->handler;
            if (alarm->period > 0) {
                /* Deliver every elapsed period (like the GameCube timer
                 * interrupt would have) so periodic work such as the 60 Hz pad
                 * sampler queues several samples when a frame runs long and the
                 * game logic catches up instead of running in slow motion.
                 * Cap the burst so a long stall does not replay forever. */
                int burst = 0;
                do {
                    alarm->fire += alarm->period;
                    handler(alarm, NULL);
                } while (alarm->handler == handler && alarm->fire <= now &&
                         ++burst < 8);
                if (alarm->handler == handler && alarm->fire <= now)
                    alarm->fire = now + alarm->period;
            } else {
                OSCancelAlarm(alarm);
                handler(alarm, NULL);
            }
        }
        alarm = next;
    }
    delivering = 0;
    melee_vita_dvd_poll();
}

u32 OSGetProgressiveMode(void) { return s_progressive; }
void OSSetProgressiveMode(u32 enabled) { s_progressive = enabled != 0; }
u32 OSGetEuRgb60Mode(void) { return s_eurgb60; }
void OSSetEuRgb60Mode(u32 enabled) { s_eurgb60 = enabled != 0; }
u32 OSGetSoundMode(void) { return s_sound_mode; }
void OSSetSoundMode(u32 mode) { s_sound_mode = mode; }
u32 OSGetResetCode(void) { return 0; }
BOOL OSCheckActiveThreads(void) { return TRUE; }
BOOL DBIsDebuggerPresent(void) { return FALSE; }

OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler)
{
    (void) error;
    (void) handler;
    return NULL;
}

void OSResetSystem(int reset, u32 reset_code, BOOL force_menu)
{
    (void) reset;
    (void) reset_code;
    (void) force_menu;
    sceKernelExitProcess(0);
}

void OSReportInit(void) {}
void OSRegisterVersion(const char* version) { (void) version; }

void OSVReport(const char* format, va_list args)
{
#ifdef MELEE_VITA_RELEASE
    (void) format;
    (void) args;
#else
    char message[768];
    va_list log_args;
    va_copy(log_args, args);
    vsnprintf(message, sizeof(message), format, log_args);
    va_end(log_args);
    melee_vita_log_info("%s", message);
    vprintf(format, args);
#endif
}

void OSVAttention(const char* format, va_list args)
{
    OSVReport(format, args);
}

void OSReport(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    OSVReport(format, args);
    va_end(args);
}

void OSPanic(const char* file, int line, const char* format, ...)
{
    va_list args;
    printf("PANIC %s:%d: ", file, line);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    sceKernelExitProcess(-1);
    abort();
}

void OSFatal(GXColor foreground, GXColor background, const char* message)
{
    (void) foreground;
    (void) background;
    OSPanic(__FILE__, __LINE__, "%s", message);
}

void DCInvalidateRange(void* address, u32 size) { (void) address; (void) size; }
void DCFlushRange(void* address, u32 size) { (void) address; (void) size; }
void DCStoreRange(void* address, u32 size) { (void) address; (void) size; }
void DCFlushRangeNoSync(void* address, u32 size) { (void) address; (void) size; }
void DCStoreRangeNoSync(void* address, u32 size) { (void) address; (void) size; }
void DCTouchRange(void* address, u32 size) { (void) address; (void) size; }
void ICInvalidateRange(void* address, u32 size) { (void) address; (void) size; }

void DCZeroRange(void* address, u32 size)
{
    if (size != 0) memset(address, 0, size);
}
