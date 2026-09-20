#include <assert.h>

#include "debug_menu_sequence.h"

int main(void)
{
    static uint64_t const sequence[] = {
        MELEE_VITA_DEBUG_UP,   MELEE_VITA_DEBUG_UP,
        MELEE_VITA_DEBUG_DOWN, MELEE_VITA_DEBUG_DOWN,
        MELEE_VITA_DEBUG_LEFT, MELEE_VITA_DEBUG_RIGHT,
        MELEE_VITA_DEBUG_LEFT, MELEE_VITA_DEBUG_RIGHT,
        MELEE_VITA_DEBUG_SELECT,
    };
    MeleeVitaDebugMenuSequence state = { 0 };
    int i;

    assert(!melee_vita_debug_menu_feed(&state, 1U << 16));
    assert(!melee_vita_debug_menu_feed(&state, MELEE_VITA_DEBUG_UP));
    assert(!melee_vita_debug_menu_feed(&state, MELEE_VITA_DEBUG_DOWN));
    assert(!melee_vita_debug_menu_feed(&state, MELEE_VITA_DEBUG_CROSS));
    assert(!state.unlocked);

    for (i = 0; i < (int) (sizeof(sequence) / sizeof(sequence[0])) - 1; i++) {
        assert(!melee_vita_debug_menu_feed(&state, sequence[i]));
    }
    assert(melee_vita_debug_menu_feed(&state, sequence[i]));
    assert(state.unlocked);
    assert(!melee_vita_debug_menu_feed(&state, MELEE_VITA_DEBUG_UP));
    return 0;
}
