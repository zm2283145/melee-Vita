#ifndef MELEE_FT_FTUCF_H
#define MELEE_FT_FTUCF_H

/* Vita: UCF 0.84 (Universal Controller Fix), ported from the Gecko codes in
 * Project Slippi's slippi-ssbm-asm, External/UCF 0.84. On while the
 * Settings screen's UCF option is (g_melee_vita_ucf). Lives in fighter.c. */
#if defined(TARGET_VITA) && !defined(TARGET_PS5)

#include <melee/ft/forward.h>

#include <Runtime/platform.h>

extern int g_melee_vita_ucf;

/* fighter input update: raw stick history for this fighter's port */
void ftUcf_RecordStick(Fighter* fp);
/* Turn, first facing flip: dashback from a tilt turn */
void ftUcf_Dashback(Fighter* fp);
/* spot dodge input: true = don't spot dodge, leave room for a shield drop */
bool ftUcf_SkipSpotDodge(Fighter* fp);
/* shield drop from shield: the stick has held the lower rim long enough */
bool ftUcf_ShieldDrop(Fighter* fp);

#endif

#endif
