/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Slippi Direct online play: the match (slippi_online.c), the menus
 * (slippi_online_mode.c) and the match overlay (slippi_online_hud.c). */
#ifndef MELEE_SLIPPI_ONLINE_H
#define MELEE_SLIPPI_ONLINE_H

#include <melee/mn/forward.h>
#include <Runtime/platform.h>
#include <dolphin/pad.h>

#ifdef MELEE_VITA_SLIPPI
/* ---- the match, slippi_online.c ---- */
/* 1 from the start of an online match until its scene exits. */
int mp_slippi_online_active(void);
/* Takes the match the two players agreed on.  1 when there was one. */
int mp_slippi_online_take_match(void);
/* The agreed game info block, from take_match until the match starts. */
const u8* mp_slippi_online_pending_block(void);
int mp_slippi_online_local_index(void);
int mp_slippi_online_delay(void);
/* 0 nothing, 1 disconnected, 2 desync detected, for the overlay. */
int mp_slippi_online_status(void);
/* 1 once the high 16 bits of a checksum differed ("Desync Risk"). */
int mp_slippi_online_desync_risk(void);

/* Hooks in the game code, at the places Slippi's online codes patch. */
int mp_slippi_online_pad_renew(PADStatus* pad);        /* controller.c */
void mp_slippi_online_frame_begin(void);               /* gmscene.c */
void mp_slippi_online_frame_end(void);                 /* gmscene.c */
void mp_slippi_tick_without_draw(void);                /* gmscene.c */
void mp_slippi_online_start_melee(StartMeleeData* d);  /* gmvs.c */
void mp_slippi_online_match_exit(void);                /* gmvs.c */

/* ---- the menus, slippi_online_mode.c ---- */
/* 1 while the online mode (GM_HANYU_CSS) runs. */
int mp_slippi_css_online(void);
/* Once per CSS frame (mncharsel.c).  Returns 1 to leave the CSS. */
int mp_slippi_css_frame(int ready, PlayerInitData* local,
                        PlayerInitData* remote, u32 trigger, u32 held);
/* 1 when the CSS comes back from the stage pick: no announcer. */
int mp_slippi_css_quiet(void);
/* Z on the online SSS (mnstagesel.c): frozen Pokemon Stadium. */
void mp_slippi_sss_toggle_alt(void);
int mp_slippi_sss_alt_mode(void);
/* In mncharsel.c: the Zelda icon plays Sheik or Zelda, and the CSS title
 * (EVENT MATCH in this layout) hides. */
void mp_slippi_css_sheik(int sheik);
void mp_slippi_css_hide_title(void);

/* In mnmain.c: Slippi's Online submenu, when its menu files are there. */
void mp_slippi_menu_prepare(void);

/* ---- the overlay, slippi_online_hud.c ---- */
/* Called from DevText_DrawAll (textdraw.c) on every screen. */
void mp_slippi_online_draw_hud(void);

#else
/* Builds without -EnableSlippi have no online play: the hooks in the game
 * code compile away and the menus stay vanilla. */
static inline int mp_slippi_online_active(void) { return 0; }
static inline int mp_slippi_online_pad_renew(PADStatus* pad)
{
    (void) pad;
    return 0;
}
static inline void mp_slippi_online_frame_begin(void) {}
static inline void mp_slippi_online_frame_end(void) {}
static inline void mp_slippi_tick_without_draw(void) {}
static inline void mp_slippi_online_start_melee(StartMeleeData* d) { (void) d; }
static inline void mp_slippi_online_match_exit(void) {}
static inline int mp_slippi_css_online(void) { return 0; }
static inline int mp_slippi_css_frame(int ready, PlayerInitData* local,
                                      PlayerInitData* remote, u32 trigger, u32 held)
{
    (void) ready;
    (void) local;
    (void) remote;
    (void) trigger;
    (void) held;
    return 0;
}
static inline int mp_slippi_css_quiet(void) { return 0; }
static inline void mp_slippi_sss_toggle_alt(void) {}
static inline void mp_slippi_menu_prepare(void) {}
static inline void mp_slippi_online_draw_hud(void) {}
#endif

#endif
