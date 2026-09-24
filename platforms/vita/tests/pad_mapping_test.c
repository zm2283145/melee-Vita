#include "../game/pad_mapping.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void test_defaults(void)
{
    uint8_t mapping[MELEE_VITA_BUTTON_COUNT];
    int i;
    melee_vita_pad_mapping_defaults(mapping);
    assert(melee_vita_pad_mapping_valid(mapping));
    assert(mapping[MELEE_VITA_BUTTON_SELECT] == MELEE_VITA_ACTION_Z);
    for (i = 0; i < MELEE_VITA_ACTION_COUNT; ++i)
        assert(mapping[i] == i);
    for (; i < MELEE_VITA_BUTTON_COUNT; ++i)
        assert(mapping[i] == MELEE_VITA_ACTION_UNBOUND);
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

static void test_extra_button_replaces_existing_binding(void)
{
    uint8_t mapping[MELEE_VITA_BUTTON_COUNT];
    melee_vita_pad_mapping_defaults(mapping);
    assert(melee_vita_pad_mapping_assign(
        mapping, MELEE_VITA_BUTTON_L2, MELEE_VITA_ACTION_Z));
    assert(mapping[MELEE_VITA_BUTTON_L2] == MELEE_VITA_ACTION_Z);
    assert(mapping[MELEE_VITA_BUTTON_SELECT] ==
           MELEE_VITA_ACTION_UNBOUND);
    assert(melee_vita_pad_mapping_valid(mapping));
}

static void test_legacy_mapping_migration(void)
{
    uint8_t mapping[MELEE_VITA_BUTTON_COUNT];
    uint8_t legacy[MELEE_VITA_LEGACY_BUTTON_COUNT];
    melee_vita_pad_mapping_defaults(mapping);
    memcpy(legacy, mapping, sizeof(legacy));
    assert(melee_vita_pad_mapping_migrate_v1(mapping, legacy));
    assert(melee_vita_pad_mapping_valid(mapping));
    assert(mapping[MELEE_VITA_BUTTON_L2] ==
           MELEE_VITA_ACTION_UNBOUND);
    assert(mapping[MELEE_VITA_BUTTON_R3] ==
           MELEE_VITA_ACTION_UNBOUND);
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
    melee_vita_pad_mapping_defaults(mapping);
    mapping[MELEE_VITA_BUTTON_L2] = MELEE_VITA_ACTION_A;
    assert(!melee_vita_pad_mapping_valid(mapping));
}

int main(void)
{
    test_defaults();
    test_assignment_swaps_duplicate();
    test_extra_button_replaces_existing_binding();
    test_legacy_mapping_migration();
    test_invalid_mapping();
    return 0;
}
