#ifndef MP_UCF_H
#define MP_UCF_H
#include <melee/ft/forward.h>
#ifdef MELEE_VITA_SLIPPI
void mp_ucf_reset(void);
/* Which UCF runs: 84 for UCF 0.84 (what Slippi runs now, and the default),
 * 80 for UCF 0.8 (what Slippi 3.14 ran, for its replays), 0 for none. */
void mp_ucf_set_version(int version);
int mp_ucf_version(void);
void mp_ucf_input(Fighter*);
void mp_ucf_dashback(Fighter*);
int mp_ucf_sdi(Fighter*);
int mp_ucf_shield_sdi(Fighter*);
int mp_ucf_tumble(Fighter*);
int mp_ucf_drop_extended(Fighter*);
int mp_ucf_suppress_spotdodge(Fighter*);
float mp_ucf_squat_threshold(Fighter*);
#else
/* Builds without -EnableSlippi: no UCF, the game's own stick handling. */
static inline void mp_ucf_reset(void) {}
static inline void mp_ucf_set_version(int version) { (void) version; }
static inline int mp_ucf_version(void) { return 0; }
static inline void mp_ucf_input(Fighter* fp) { (void) fp; }
static inline void mp_ucf_dashback(Fighter* fp) { (void) fp; }
static inline int mp_ucf_sdi(Fighter* fp)
{
    (void) fp;
    return 0;
}
static inline int mp_ucf_shield_sdi(Fighter* fp)
{
    (void) fp;
    return 0;
}
static inline int mp_ucf_tumble(Fighter* fp)
{
    (void) fp;
    return 0;
}
static inline int mp_ucf_drop_extended(Fighter* fp)
{
    (void) fp;
    return 0;
}
static inline int mp_ucf_suppress_spotdodge(Fighter* fp)
{
    (void) fp;
    return 0;
}
/* The vanilla threshold (ftCo_SquatRv.c has p_ftCommonData). */
#define mp_ucf_squat_threshold(fp) (p_ftCommonData->x94)
#endif
#endif
