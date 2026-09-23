#include "../game/pad_mapping.h"

#include <assert.h>
#include <stdint.h>

static void test_defaults(void)
{
    uint8_t mapping[MELEE_VITA_BUTTON_COUNT];
    int i;
    melee_vita_pad_mapping_defaults(mapping);
    assert(melee_vita_pad_mapping_valid(mapping));
    for (i = 0; i < MELEE_VITA_BUTTON_COUNT; ++i)
        assert(mapping[i] == i);
}

static void test_assignment_swaps_duplicate(void)
{
    uint8_t mapping[MELEE_VITA_BUTTON_COUNT];
    melee_vita_pad_mapping_defaults(mapping);
    assert(melee_vita_pad_mapping_assign(
        mapping, MELEE_VITA_BUTTON_CROSS, MELEE_VITA_ACTION_Z));
    assert(mapping[MELEE_VITA_BUTTON_CROSS] == MELEE_VITA_ACTION_Z);
    assert(mapping[MELEE_VITA_BUTTON_SELECT] == MELEE_VITA_ACTION_A);
    assert(melee_vita_pad_mapping_valid(mapping));
}

static void test_invalid_mapping(void)
{
    uint8_t mapping[MELEE_VITA_BUTTON_COUNT];
    melee_vita_pad_mapping_defaults(mapping);
    mapping[MELEE_VITA_BUTTON_CIRCLE] = MELEE_VITA_ACTION_A;
    assert(!melee_vita_pad_mapping_valid(mapping));
    assert(!melee_vita_pad_mapping_assign(
        mapping, MELEE_VITA_BUTTON_CROSS, MELEE_VITA_ACTION_B));
    melee_vita_pad_mapping_defaults(mapping);
    assert(!melee_vita_pad_mapping_assign(mapping, -1, MELEE_VITA_ACTION_A));
    assert(!melee_vita_pad_mapping_assign(
        mapping, MELEE_VITA_BUTTON_CROSS, MELEE_VITA_ACTION_COUNT));
}

int main(void)
{
    test_defaults();
    test_assignment_swaps_duplicate();
    test_invalid_mapping();
    return 0;
}
