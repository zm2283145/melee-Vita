#ifndef MELEE_GM_GMMENUMODE_RETURN_H
#define MELEE_GM_GMMENUMODE_RETURN_H

#include <melee/gm/forward.h>
#include <melee/mn/forward.h>

static inline RegMatchMenuSelection
gmMenuMode_AllStarReturnSelection(GameModeKind previous_mode,
                                  bool allstar_unlocked)
{
    if ((previous_mode == GM_ALLSTAR ||
         previous_mode == GM_ALLSTAR_GOVER) &&
        allstar_unlocked)
    {
        return SEL_REG_ALLSTAR;
    }
    return SEL_REG_CLASSIC;
}

#endif
