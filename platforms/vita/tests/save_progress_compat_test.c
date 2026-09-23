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

static void test_imported_stadium_records_are_classified_and_repaired(void)
{
    /* Values decoded from a GameCube save exported by the Vita. Mewtwo's
     * original max combo was 11, displayed as 2816 before this repair. */
    uint16_t combo = 0x0B00;
    uint32_t target_frames = UINT32_C(0x35040000);
    uint32_t home_run_distance = UINT32_C(0xAAAA0000);
    uint32_t home_run_score = UINT32_C(0xAAAA0000);
    uint32_t combo_leaderboard = UINT32_C(0x0B000000);
    uint32_t completion_mask = UINT32_C(0xFFFFFF01);

    assert(melee_vita_classify_record_u16(
               combo, MELEE_VITA_STADIUM_COMBO_MAX) ==
           MELEE_VITA_SAVE_BIG_ENDIAN);
    assert(melee_vita_classify_record_u32(
               target_frames, MELEE_VITA_STADIUM_RECORD_MAX) ==
           MELEE_VITA_SAVE_BIG_ENDIAN);
    assert(melee_vita_has_imported_record_evidence(3, 0));
    assert(!melee_vita_has_imported_record_evidence(2, 0));
    assert(!melee_vita_has_imported_record_evidence(4, 2));

    combo = melee_vita_swap_u16(combo);
    assert(combo == 11);
    assert(melee_vita_normalize_record_u32(
        &target_frames, MELEE_VITA_STADIUM_RECORD_MAX));
    assert(target_frames == 1077);
    assert(melee_vita_normalize_record_u32(
        &home_run_distance, MELEE_VITA_STADIUM_RECORD_MAX));
    assert(home_run_distance == 43690);
    assert(melee_vita_normalize_record_u32(
        &home_run_score, MELEE_VITA_STADIUM_RECORD_MAX));
    assert(home_run_score == 43690);
    assert(melee_vita_normalize_record_u32(
        &combo_leaderboard, MELEE_VITA_STADIUM_RECORD_MAX));
    assert(combo_leaderboard == 11);
    assert(melee_vita_normalize_fighter_mask(&completion_mask));
    assert(completion_mask == UINT32_C(0x01FFFFFF));
    assert(melee_vita_convert_fighter_record_flags(0x69FF) == 0x36FF);
}

static void test_native_stadium_records_remain_unchanged(void)
{
    uint32_t target_frames = 1077;
    uint32_t completion_mask = UINT32_C(0x01FFFFFF);
    uint32_t empty_record = 0;
    uint32_t home_run_distance_from_partly_repaired_save =
        UINT32_C(0xAAAA0000);

    assert(melee_vita_classify_record_u16(
               11, MELEE_VITA_STADIUM_COMBO_MAX) ==
           MELEE_VITA_SAVE_NATIVE);
    assert(!melee_vita_normalize_record_u32(
        &target_frames, MELEE_VITA_STADIUM_RECORD_MAX));
    assert(!melee_vita_normalize_fighter_mask(&completion_mask));
    assert(!melee_vita_normalize_record_u32(
        &empty_record, MELEE_VITA_STADIUM_RECORD_MAX));
    assert(melee_vita_normalize_record_u32(
        &home_run_distance_from_partly_repaired_save,
        MELEE_VITA_STADIUM_RECORD_MAX));
    assert(!melee_vita_normalize_record_u32(
        &home_run_distance_from_partly_repaired_save,
        MELEE_VITA_STADIUM_RECORD_MAX));
    assert(target_frames == 1077);
    assert(completion_mask == UINT32_C(0x01FFFFFF));
    assert(empty_record == 0);
    assert(home_run_distance_from_partly_repaired_save == 43690);
}

static void test_imported_mode_scores_are_repaired_once(void)
{
    uint32_t scores[3][3] = {
        { UINT32_C(0x40420F00), UINT32_C(0x80B51800), 0 },
        { UINT32_C(0xC0270900), UINT32_C(0x00471C00), 0 },
        { 0, 0, 0 },
    };

    assert(melee_vita_normalize_mode_scores(scores, 3));
    assert(scores[0][0] == 1000000);
    assert(scores[0][1] == 1619328);
    assert(scores[1][0] == 600000);
    assert(scores[1][1] == 1853184);
    assert(!melee_vita_normalize_mode_scores(scores, 3));
}

static void test_native_mode_scores_and_weak_evidence_are_untouched(void)
{
    uint32_t native_scores[3][3] = {
        { 1000000, 1627520, 0 },
        { 600000, 9216, 0 },
        { 0, 0, 0 },
    };
    uint32_t weak_evidence[2][3] = {
        { UINT32_C(0x40420F00), 0, 0 },
        { 0, 0, 0 },
    };

    assert(!melee_vita_normalize_mode_scores(native_scores, 3));
    assert(native_scores[0][0] == 1000000);
    assert(!melee_vita_normalize_mode_scores(weak_evidence, 2));
    assert(weak_evidence[0][0] == UINT32_C(0x40420F00));
}

int main(void)
{
    test_native_progress_is_unchanged();
    test_gamecube_progress_is_normalized();
    test_partially_normalized_save_is_repaired_once();
    test_invalid_progress_is_untouched();
    test_imported_stadium_records_are_classified_and_repaired();
    test_native_stadium_records_remain_unchanged();
    test_imported_mode_scores_are_repaired_once();
    test_native_mode_scores_and_weak_evidence_are_untouched();
    return 0;
}
