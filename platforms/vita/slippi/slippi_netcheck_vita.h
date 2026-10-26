/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Boot-time network check for Slippi Direct play bring-up. */
#pragma once

/* Runs the check if ux0:data/melee/slippi/netcheck.txt exists, and writes
 * the result to ux0:data/melee/slippi/slippi.log.  Does nothing otherwise. */
void melee_vita_slippi_netcheck(void);
