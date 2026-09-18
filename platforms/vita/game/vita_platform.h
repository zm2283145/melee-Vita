/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MELEE_VITA_PLATFORM_H
#define MELEE_VITA_PLATFORM_H

#include <dolphin/types.h>

#define MELEE_VITA_MEM1_SIZE (24u * 1024u * 1024u)
#define MELEE_VITA_ARAM_SIZE (16u * 1024u * 1024u)

/* Called by the Vita entry point before the original game's melee_main(). */
int melee_vita_platform_init(void);
void melee_vita_platform_shutdown(void);

/* Called at the future VI frame boundary so GameCube alarms execute on the
 * game thread, matching the original interrupt-driven behavior. */
void melee_vita_os_run_alarms(void);
/* Dispatch deferred emulated-hardware completions from game wait loops that do
 * not cross a VI retrace. */
void melee_vita_platform_poll(void);
void melee_vita_dvd_poll(void);
void melee_vita_dvd_log_stats(const char* phase);
void melee_vita_dvd_shutdown(void);
void melee_vita_audio_poll(void);
void melee_vita_audio_flush(void);
void melee_vita_audio_shutdown(void);
void melee_vita_card_poll(void);

#endif
