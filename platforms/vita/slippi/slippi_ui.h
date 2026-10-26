/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The online CSS session (slippi_ui.c): the Vita-only calls.  The calls the
 * game makes are in src/melee/slippi/slippi_platform.h. */
#ifndef MELEE_VITA_SLIPPI_UI_H
#define MELEE_VITA_SLIPPI_UI_H

#include <dolphin/pad.h>

/* Called from PADRead (game/pad.c) with port 1's pad.  While the connect
 * code keyboard is open the game gets an empty pad, and while locked in on
 * the online CSS, A, B, X and Y are taken out so the fighter stays picked. */
void melee_vita_slippi_pad_filter(PADStatus* pad);

/* Called by the renderer (game/gxm_game.c) after each frame, on the render
 * thread.  Draws the system keyboard while it is open. */
void melee_vita_slippi_render_dialog(void);

#endif
