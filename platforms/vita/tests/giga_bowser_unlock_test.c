#include <assert.h>
#include <stdint.h>

#include "melee/gm/giga_bowser_rules.h"

static void test_event_51_eligibility(void)
{
    uint64_t const event_51 = UINT64_C(1) << GM_GIGA_BOWSER_EVENT_INDEX;

    assert(!gmGigaBowser_IsEligible(0, false, 0));
    assert(gmGigaBowser_IsEligible(event_51, false, 0));
    assert(gmGigaBowser_IsEligible(0, true, 0));
    assert(!gmGigaBowser_IsEligible(
        event_51, true, GM_GIGA_BOWSER_SAVE_FLAG_UNLOCKED));
}

static void test_win_and_loss_unlock_behavior(void)
{
    uint8_t flags = 0x54;

    assert(gmGigaBowser_ApplyChallengerResult(flags, false) == flags);
    flags = gmGigaBowser_ApplyChallengerResult(flags, true);
    assert(gmGigaBowser_IsUnlocked(flags));
    assert((flags & 0x54) == 0x54);
}

static void test_save_persistence_and_compatibility(void)
{
    uint16_t const legacy_character_unlocks = UINT16_MAX;
    uint8_t stored_flags = 0;

    assert(legacy_character_unlocks == UINT16_MAX);
    assert(!gmGigaBowser_IsUnlocked(stored_flags));
    stored_flags = gmGigaBowser_ApplyChallengerResult(stored_flags, true);
    assert(stored_flags == GM_GIGA_BOWSER_SAVE_FLAG_UNLOCKED);
    assert(gmGigaBowser_IsUnlocked(stored_flags));
}

static void test_css_visibility(void)
{
    uint8_t const unlocked = GM_GIGA_BOWSER_SAVE_FLAG_UNLOCKED;

    assert(!gmGigaBowser_IsCssVisible(0x00, 0));
    assert(gmGigaBowser_IsCssVisible(0x00, unlocked));
    assert(gmGigaBowser_IsCssVisible(0x0A, unlocked));
    assert(gmGigaBowser_IsCssVisible(0x0B, unlocked));
    assert(gmGigaBowser_IsCssVisible(0x0C, unlocked));
    assert(gmGigaBowser_IsCssVisible(0x0D, unlocked));
    assert(gmGigaBowser_IsCssVisible(0x17, unlocked));
    assert(!gmGigaBowser_IsCssVisible(0x0E, unlocked));
}

static void test_css_maps_to_giga_bowser_fighter(void)
{
    assert(gmGigaBowser_FighterKindForCssSelection(
               GM_GIGA_BOWSER_CSS_CKIND) == GM_GIGA_BOWSER_FIGHTER_KIND);
    assert(gmGigaBowser_FighterKindForCssSelection(5) == UINT8_MAX);
}

int main(void)
{
    test_event_51_eligibility();
    test_win_and_loss_unlock_behavior();
    test_save_persistence_and_compatibility();
    test_css_visibility();
    test_css_maps_to_giga_bowser_fighter();
    return 0;
}
