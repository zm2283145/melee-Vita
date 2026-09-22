#include <assert.h>
#include <stdint.h>

#include "melee_save_compat.h"

static void test_native_progress_is_unchanged(void)
{
    MeleeVitaProgressFields fields = {
        0x17FF,
        0x0FFF,
        UINT64_C(0x0007FFFFFFFFFFFF),
    };

    assert(melee_vita_normalize_progress_fields(&fields) ==
           MELEE_VITA_SAVE_NATIVE);
    assert(fields.unlocked_characters == 0x17FF);
    assert(fields.unlocked_stages == 0x0FFF);
    assert(fields.completed_events == UINT64_C(0x0007FFFFFFFFFFFF));
}

static void test_gamecube_progress_is_normalized(void)
{
    /* Values seen by the little-endian Vita after loading a 100% GCI. */
    MeleeVitaProgressFields fields = {
        0xFF17,
        0xFF0F,
        UINT64_C(0xFFFFFFFFFFFF0700),
    };

    assert(melee_vita_normalize_progress_fields(&fields) ==
           MELEE_VITA_SAVE_BIG_ENDIAN);
    assert(fields.unlocked_characters == 0x17FF);
    assert(fields.unlocked_stages == 0x0FFF);
    assert(fields.completed_events == UINT64_C(0x0007FFFFFFFFFFFF));
    assert((fields.completed_events & (UINT64_C(1) << 50)) != 0);
}

static void test_partially_normalized_save_is_repaired_once(void)
{
    MeleeVitaProgressFields fields = {
        0x17FF,
        0xFF0F,
        UINT64_C(0xFFFFFFFFFFFF0700),
    };

    assert(melee_vita_normalize_progress_fields(&fields) ==
           MELEE_VITA_SAVE_BIG_ENDIAN);
    assert(fields.unlocked_characters == 0x17FF);
    assert(fields.unlocked_stages == 0x0FFF);
    assert(fields.completed_events == UINT64_C(0x0007FFFFFFFFFFFF));
    assert(melee_vita_normalize_progress_fields(&fields) ==
           MELEE_VITA_SAVE_NATIVE);
}

static void test_invalid_progress_is_untouched(void)
{
    MeleeVitaProgressFields fields = {
        0xFF17,
        0xF0F0,
        UINT64_C(0xFFFFFFFFFFFF0700),
    };
    MeleeVitaProgressFields original = fields;

    assert(melee_vita_normalize_progress_fields(&fields) ==
           MELEE_VITA_SAVE_INVALID);
    assert(fields.unlocked_characters == original.unlocked_characters);
    assert(fields.unlocked_stages == original.unlocked_stages);
    assert(fields.completed_events == original.completed_events);
}

int main(void)
{
    test_native_progress_is_unchanged();
    test_gamecube_progress_is_normalized();
    test_partially_normalized_save_is_repaired_once();
    test_invalid_progress_is_untouched();
    return 0;
}
