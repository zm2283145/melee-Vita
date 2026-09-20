#ifndef MELEE_MN_VITADEBUG_H
#define MELEE_MN_VITADEBUG_H

#include <Runtime/platform.h>

bool mnVitaDebug_FeedActivation(u64 triggers);
bool mnVitaDebug_IsUnlocked(void);
bool mnVitaDebug_IsActive(void);
void mnVitaDebug_Begin(void);
void mnVitaDebug_End(void);

#endif
