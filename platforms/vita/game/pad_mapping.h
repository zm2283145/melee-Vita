/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MELEE_VITA_PAD_MAPPING_H
#define MELEE_VITA_PAD_MAPPING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum MeleeVitaPhysicalButton {
    MELEE_VITA_BUTTON_CROSS = 0,
    MELEE_VITA_BUTTON_CIRCLE,
    MELEE_VITA_BUTTON_SQUARE,
    MELEE_VITA_BUTTON_TRIANGLE,
    MELEE_VITA_BUTTON_L,
    MELEE_VITA_BUTTON_R,
    MELEE_VITA_BUTTON_SELECT,
    MELEE_VITA_BUTTON_START,
    MELEE_VITA_BUTTON_COUNT,
} MeleeVitaPhysicalButton;

typedef enum MeleeVitaPadAction {
    MELEE_VITA_ACTION_A = 0,
    MELEE_VITA_ACTION_B,
    MELEE_VITA_ACTION_X,
    MELEE_VITA_ACTION_Y,
    MELEE_VITA_ACTION_L,
    MELEE_VITA_ACTION_R,
    MELEE_VITA_ACTION_Z,
    MELEE_VITA_ACTION_START,
    MELEE_VITA_ACTION_COUNT,
} MeleeVitaPadAction;

static inline void melee_vita_pad_mapping_defaults(
    uint8_t mapping[MELEE_VITA_BUTTON_COUNT])
{
    static const uint8_t defaults[MELEE_VITA_BUTTON_COUNT] = {
        MELEE_VITA_ACTION_A,
        MELEE_VITA_ACTION_B,
        MELEE_VITA_ACTION_X,
        MELEE_VITA_ACTION_Y,
        MELEE_VITA_ACTION_L,
        MELEE_VITA_ACTION_R,
        MELEE_VITA_ACTION_Z,
        MELEE_VITA_ACTION_START,
    };
    int i;
    for (i = 0; i < MELEE_VITA_BUTTON_COUNT; ++i)
        mapping[i] = defaults[i];
}

static inline bool melee_vita_pad_mapping_valid(
    const uint8_t mapping[MELEE_VITA_BUTTON_COUNT])
{
    uint32_t seen = 0;
    int i;
    if (mapping == NULL)
        return false;
    for (i = 0; i < MELEE_VITA_BUTTON_COUNT; ++i) {
        uint8_t const action = mapping[i];
        uint32_t bit;
        if (action >= MELEE_VITA_ACTION_COUNT)
            return false;
        bit = 1u << action;
        if ((seen & bit) != 0u)
            return false;
        seen |= bit;
    }
    return seen == (1u << MELEE_VITA_ACTION_COUNT) - 1u;
}

static inline bool melee_vita_pad_mapping_assign(
    uint8_t mapping[MELEE_VITA_BUTTON_COUNT], int physical_button, int action)
{
    int other;
    uint8_t previous;
    if (!melee_vita_pad_mapping_valid(mapping) ||
        physical_button < 0 || physical_button >= MELEE_VITA_BUTTON_COUNT ||
        action < 0 || action >= MELEE_VITA_ACTION_COUNT)
    {
        return false;
    }
    previous = mapping[physical_button];
    if (previous == action)
        return true;
    for (other = 0; other < MELEE_VITA_BUTTON_COUNT; ++other) {
        if (mapping[other] == action)
            break;
    }
    if (other == MELEE_VITA_BUTTON_COUNT)
        return false;
    mapping[other] = previous;
    mapping[physical_button] = (uint8_t) action;
    return true;
}

#endif
