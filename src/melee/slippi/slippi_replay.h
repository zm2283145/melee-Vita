/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Slippi replay test: plays a recorded match and compares every frame. */
#ifndef MELEE_SLIPPI_REPLAY_H
#define MELEE_SLIPPI_REPLAY_H

#include <melee/ft/forward.h>
#include <melee/mn/forward.h>
#include <Runtime/platform.h>

#ifdef MELEE_VITA_SLIPPI
/* Everything here does nothing unless ux0:data/melee/slippi/replay.bin or
 * replay1.bin exists (made by tools/slippi/vita_replay_pack.py). */
int mp_slippi_replay_on(void);

/* Hooks in the game code, at the places Slippi's playback codes patch. */
void mp_slippi_replay_boot_mode(u8* mode);             /* gm_1A3F.c */
void mp_slippi_replay_prepare_scene(void);             /* gm_1A3F.c */
void mp_slippi_replay_start_melee(StartMeleeData* d);  /* gmvs.c */
void mp_slippi_replay_scene_think(int match_result);   /* gmvs.c */
int mp_slippi_replay_terminated(void);                 /* gmvs.c */
void mp_slippi_replay_match_exit(void);                /* gmvs.c */
void mp_slippi_replay_input(Fighter* fp);              /* fighter.c */
void mp_slippi_replay_post_frame(Fighter* fp);         /* fighter.c */
void mp_slippi_replay_route(u8* next_state_id);        /* gm_1A3F.c */
void mp_slippi_replay_draw_panel(void);                /* textdraw.c */

#else
/* Builds without -EnableSlippi have no replay test. */
static inline int mp_slippi_replay_on(void) { return 0; }
static inline void mp_slippi_replay_boot_mode(u8* mode) { (void) mode; }
static inline void mp_slippi_replay_prepare_scene(void) {}
static inline void mp_slippi_replay_start_melee(StartMeleeData* d) { (void) d; }
static inline void mp_slippi_replay_scene_think(int match_result) { (void) match_result; }
static inline int mp_slippi_replay_terminated(void) { return 0; }
static inline void mp_slippi_replay_match_exit(void) {}
static inline void mp_slippi_replay_input(Fighter* fp) { (void) fp; }
static inline void mp_slippi_replay_post_frame(Fighter* fp) { (void) fp; }
static inline void mp_slippi_replay_route(u8* next_state_id) { (void) next_state_id; }
static inline void mp_slippi_replay_draw_panel(void) {}
#endif

#endif
