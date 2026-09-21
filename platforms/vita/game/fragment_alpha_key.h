#ifndef MELEE_VITA_FRAGMENT_ALPHA_KEY_H
#define MELEE_VITA_FRAGMENT_ALPHA_KEY_H

#include <stdbool.h>

#include <dolphin/gx/GXEnum.h>
#include <dolphin/types.h>

static inline bool melee_vita_alpha_test_trivially_passes(
    u8 operation, u8 compare0, u8 compare1)
{
    return
        (operation == GX_AOP_AND && compare0 == GX_ALWAYS &&
         compare1 == GX_ALWAYS) ||
        (operation == GX_AOP_OR &&
         (compare0 == GX_ALWAYS || compare1 == GX_ALWAYS));
}

static inline void melee_vita_normalize_alpha_shader_refs(u8 references[2])
{
    references[0] = 0u;
    references[1] = 0u;
}

#endif
