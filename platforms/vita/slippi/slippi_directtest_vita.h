/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Boot-time Direct connect test against a real Slippi Dolphin player. */
#pragma once

/* Runs the test if ux0:data/melee/slippi/directtest.txt exists.  The file
 * holds the opponent's connect code, for example "ABCD#123".  The result is
 * written to ux0:data/melee/slippi/slippi.log.  Does nothing otherwise. */
void melee_vita_slippi_directtest(void);
