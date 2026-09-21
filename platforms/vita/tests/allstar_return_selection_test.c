#include <assert.h>

#include "melee/gm/gmmenumode_return.h"

int main(void)
{
    assert(gmMenuMode_AllStarReturnSelection(GM_ALLSTAR, false) ==
           SEL_REG_CLASSIC);
    assert(gmMenuMode_AllStarReturnSelection(GM_ALLSTAR_GOVER, false) ==
           SEL_REG_CLASSIC);
    assert(gmMenuMode_AllStarReturnSelection(GM_ALLSTAR, true) ==
           SEL_REG_ALLSTAR);
    assert(gmMenuMode_AllStarReturnSelection(GM_ALLSTAR_GOVER, true) ==
           SEL_REG_ALLSTAR);
    return 0;
}
