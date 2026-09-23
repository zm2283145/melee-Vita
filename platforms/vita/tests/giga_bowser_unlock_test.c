#include <assert.h>
#include <stdint.h>

#include "melee/gm/giga_bowser_rules.h"
#include "melee/ft/kinds/ftGigaKoopa/costume_metadata.h"

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
    /* A legacy save has no versioned Giga record. Reopening a current save
     * must preserve its independent scores. */
    assert(gmGigaBowser_RecordNeedsInitialization(0, 0));
    assert(gmGigaBowser_RecordNeedsInitialization(
        GM_GIGA_BOWSER_RECORD_MAGIC, 0));
    assert(!gmGigaBowser_RecordNeedsInitialization(
        GM_GIGA_BOWSER_RECORD_MAGIC, GM_GIGA_BOWSER_RECORD_VERSION));
}

static void test_records_are_separate_from_bowser(void)
{
    uint8_t const bowser = gmGigaBowser_RecordIndex(
        GM_GIGA_BOWSER_BOWSER_CKIND, 5);
    uint8_t const giga = gmGigaBowser_RecordIndex(
        GM_GIGA_BOWSER_CSS_CKIND, 5);

    assert(bowser == 5);
    assert(giga == 25);
    assert(giga != bowser);
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
    for (uint8_t mode = 0x0F; mode <= 0x17; mode++) {
        assert(!gmGigaBowser_IsCssVisible(mode, 0));
        assert(gmGigaBowser_IsCssVisible(mode, unlocked));
    }
    assert(!gmGigaBowser_IsCssVisible(0x0E, 0));
    assert(gmGigaBowser_IsCssVisible(0x0E, unlocked));
    assert(!gmGigaBowser_IsCssVisible(0x18, unlocked));
}

static void test_adventure_cutscenes_use_bowser_only_for_giga(void)
{
    uint8_t const cutscenes[] = { 0x1A, 0x22, 0x24, 0x52, 0x5A, 0x5B, 0x5D };
    for (unsigned i = 0; i < sizeof(cutscenes); i++) {
        assert(gmGigaBowser_AdventureCutsceneKind(
                   cutscenes[i], GM_GIGA_BOWSER_CSS_CKIND) ==
               GM_GIGA_BOWSER_BOWSER_CKIND);
        assert(gmGigaBowser_AdventureCutsceneColor(
                   cutscenes[i], GM_GIGA_BOWSER_CSS_CKIND, 5) == 0);
    }
    assert(gmGigaBowser_AdventureCutsceneKind(0x1A, 1) == 1);
    assert(gmGigaBowser_AdventureCutsceneColor(0x1A, 1, 3) == 3);
    assert(gmGigaBowser_AdventureCutsceneKind(0x19,
           GM_GIGA_BOWSER_CSS_CKIND) == GM_GIGA_BOWSER_CSS_CKIND);
}

static void test_css_maps_to_giga_bowser_fighter(void)
{
    assert(gmGigaBowser_FighterKindForCssSelection(
               GM_GIGA_BOWSER_CSS_CKIND) == GM_GIGA_BOWSER_FIGHTER_KIND);
    assert(gmGigaBowser_FighterKindForCssSelection(5) == UINT8_MAX);
}

static void test_giga_costume_uses_shared_fighter_metadata(void)
{
    for (uint8_t costume = 0; costume < 6; costume++) {
        assert(ftGk_MetadataCostumeId(true, costume) == 0);
        assert(ftGk_MetadataCostumeId(false, costume) == costume);
    }
}

int main(void)
{
    test_event_51_eligibility();
    test_win_and_loss_unlock_behavior();
    test_save_persistence_and_compatibility();
    test_records_are_separate_from_bowser();
    test_css_visibility();
    test_adventure_cutscenes_use_bowser_only_for_giga();
    test_css_maps_to_giga_bowser_fighter();
    test_giga_costume_uses_shared_fighter_metadata();
    return 0;
}
