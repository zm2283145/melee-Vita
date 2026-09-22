#include <assert.h>
#include <string.h>

#include "vita_build_label.h"

int main(void)
{
    assert(strcmp(MELEE_VITA_BUILD_LABEL, "VITA 0.8.2 BUILD 37.1") == 0);
    assert(sizeof(MELEE_VITA_BUILD_LABEL) <= 49);
    return 0;
}
