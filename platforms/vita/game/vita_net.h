/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shared SceNet start/stop for everything in the game that opens sockets. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Starts SceNet and SceNetCtl the first time it is called, and only counts
 * the caller after that.  Returns 0, or a negative SceNet error code. */
int melee_vita_net_start(void);

/* Call once for every melee_vita_net_start() that returned 0.  The last
 * caller shuts SceNet down again. */
void melee_vita_net_stop(void);

#ifdef __cplusplus
}
#endif
