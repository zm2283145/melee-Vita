#ifndef MELEE_VITA_DEBUG_MENU_SEQUENCE_H
#define MELEE_VITA_DEBUG_MENU_SEQUENCE_H

#include <stdbool.h>
#include <stdint.h>

enum MeleeVitaDebugMenuInput {
    MELEE_VITA_DEBUG_LEFT = 1U << 0,
    MELEE_VITA_DEBUG_RIGHT = 1U << 1,
    MELEE_VITA_DEBUG_DOWN = 1U << 2,
    MELEE_VITA_DEBUG_UP = 1U << 3,
    MELEE_VITA_DEBUG_SELECT = 1U << 4,
    MELEE_VITA_DEBUG_R = 1U << 5,
    MELEE_VITA_DEBUG_L = 1U << 6,
    MELEE_VITA_DEBUG_CROSS = 1U << 8,
    MELEE_VITA_DEBUG_CIRCLE = 1U << 9,
    MELEE_VITA_DEBUG_SQUARE = 1U << 10,
    MELEE_VITA_DEBUG_TRIANGLE = 1U << 11,
    MELEE_VITA_DEBUG_START = 1U << 12,
};

typedef struct MeleeVitaDebugMenuSequence {
    uint8_t step;
    bool unlocked;
} MeleeVitaDebugMenuSequence;

static inline bool melee_vita_debug_menu_feed(
    MeleeVitaDebugMenuSequence* state, uint64_t triggers)
{
    static uint64_t const sequence[] = {
        MELEE_VITA_DEBUG_UP,   MELEE_VITA_DEBUG_UP,
        MELEE_VITA_DEBUG_DOWN, MELEE_VITA_DEBUG_DOWN,
        MELEE_VITA_DEBUG_LEFT, MELEE_VITA_DEBUG_RIGHT,
        MELEE_VITA_DEBUG_LEFT, MELEE_VITA_DEBUG_RIGHT,
        MELEE_VITA_DEBUG_SELECT,
    };
    uint64_t const physical_buttons =
        MELEE_VITA_DEBUG_LEFT | MELEE_VITA_DEBUG_RIGHT |
        MELEE_VITA_DEBUG_DOWN | MELEE_VITA_DEBUG_UP |
        MELEE_VITA_DEBUG_SELECT | MELEE_VITA_DEBUG_R | MELEE_VITA_DEBUG_L |
        MELEE_VITA_DEBUG_CROSS | MELEE_VITA_DEBUG_CIRCLE |
        MELEE_VITA_DEBUG_SQUARE | MELEE_VITA_DEBUG_TRIANGLE |
        MELEE_VITA_DEBUG_START;
    uint64_t pressed = triggers & physical_buttons;

    if (state->unlocked || pressed == 0) {
        return false;
    }
    if (pressed == sequence[state->step]) {
        state->step++;
        if (state->step == sizeof(sequence) / sizeof(sequence[0])) {
            state->step = 0;
            state->unlocked = true;
            return true;
        }
    } else {
        state->step = pressed == sequence[0] ? 1 : 0;
    }
    return false;
}

#endif
