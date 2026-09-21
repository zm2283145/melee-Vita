#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "melee_save_compat.h"
#include "melee/ty/tyfigupon_utils.h"

#define TROPHY_COUNT 293

static void test_native_data_is_unchanged(void)
{
    uint16_t flags[TROPHY_COUNT];
    uint16_t original_flags[TROPHY_COUNT];
    MeleeVitaTrophyLotteryFields fields = {
        TROPHY_COUNT,
        0x00F4,
        9812,
        144695,
        144695,
    };
    MeleeVitaTrophyLotteryFields original = fields;
    size_t i;

    for (i = 0; i < TROPHY_COUNT; i++) {
        flags[i] = (uint16_t) (0x4000U | i);
    }
    memcpy(original_flags, flags, sizeof(flags));

    assert(melee_vita_normalize_trophy_lottery_fields(
               &fields, flags, TROPHY_COUNT) == MELEE_VITA_SAVE_NATIVE);
    assert(memcmp(&fields, &original, sizeof(fields)) == 0);
    assert(memcmp(flags, original_flags, sizeof(flags)) == 0);
}

static void test_reporter_tuple_is_normalized_once(void)
{
    uint16_t flags[TROPHY_COUNT];
    MeleeVitaTrophyLotteryFields fields = {
        0x2501,
        0xF400,
        0x54260000,
        0x37350200,
        0x37350200,
    };
    size_t i;

    for (i = 0; i < TROPHY_COUNT; i++) {
        flags[i] = (uint16_t) (0x6300U + ((i & 1U) << 8));
    }

    assert(melee_vita_normalize_trophy_lottery_fields(
               &fields, flags, TROPHY_COUNT) == MELEE_VITA_SAVE_BIG_ENDIAN);
    assert(fields.trophy_count == 293);
    assert(fields.trophy_categories == 0x00F4);
    assert(fields.current_coins == 9812);
    assert(fields.lifetime_coins == 144695);
    assert(fields.session_coins == 144695);
    for (i = 0; i < TROPHY_COUNT; i++) {
        assert(flags[i] == (uint16_t) (0x0063U + (i & 1U)));
    }

    assert(melee_vita_normalize_trophy_lottery_fields(
               &fields, flags, TROPHY_COUNT) == MELEE_VITA_SAVE_NATIVE);
    assert(fields.current_coins / 10U == 981);
}

static void simulate_legacy_gallery_normalization(
    MeleeVitaTrophyLotteryFields* fields, uint16_t* flags)
{
    size_t i;

    if (fields->trophy_count <= TROPHY_COUNT ||
        melee_vita_swap_u16(fields->trophy_count) > TROPHY_COUNT)
    {
        return;
    }
    fields->trophy_count = melee_vita_swap_u16(fields->trophy_count);
    fields->trophy_categories =
        melee_vita_swap_u16(fields->trophy_categories);
    for (i = 0; i < TROPHY_COUNT; i++) {
        flags[i] = melee_vita_swap_u16(flags[i]);
    }
}

static void test_gallery_normalized_reporter_state_recovers_coins(void)
{
    uint16_t flags[TROPHY_COUNT];
    uint16_t original_flags[TROPHY_COUNT];
    MeleeVitaTrophyLotteryFields fields = {
        0x2501,
        0xF400,
        0x54260000,
        0x37350200,
        0x37350200,
    };
    size_t i;

    for (i = 0; i < TROPHY_COUNT; i++) {
        flags[i] = (uint16_t) (0x6300U + ((i & 1U) << 8));
    }
    simulate_legacy_gallery_normalization(&fields, flags);
    memcpy(original_flags, flags, sizeof(flags));

    assert(melee_vita_classify_trophy_fields(
               fields.trophy_count,
               fields.trophy_categories) == MELEE_VITA_SAVE_NATIVE);
    assert(melee_vita_classify_lottery_coins(fields.current_coins) ==
           MELEE_VITA_SAVE_BIG_ENDIAN);
    assert(melee_vita_normalize_trophy_lottery_fields(
               &fields, flags, TROPHY_COUNT) == MELEE_VITA_SAVE_BIG_ENDIAN);
    assert(fields.trophy_count == 293);
    assert(fields.trophy_categories == 0x00F4);
    assert(memcmp(flags, original_flags, sizeof(flags)) == 0);
    assert(fields.current_coins == 9812);
    assert(fields.lifetime_coins == 144695);
    assert(fields.session_coins == 144695);
    assert(fields.current_coins / 10U == 981);

    assert(melee_vita_normalize_trophy_lottery_fields(
               &fields, flags, TROPHY_COUNT) == MELEE_VITA_SAVE_NATIVE);
}

static void test_normalization_precedes_native_trophy_mutation(void)
{
    uint16_t flags[TROPHY_COUNT] = { 0 };
    MeleeVitaTrophyLotteryFields late = {
        0x2501,
        0xF400,
        0x54260000,
        0x37350200,
        0x37350200,
    };
    MeleeVitaTrophyLotteryFields early = late;

    late.trophy_categories |= 4;
    assert(melee_vita_classify_trophy_fields(
                   late.trophy_count,
                   late.trophy_categories) == MELEE_VITA_SAVE_INVALID);
    assert(melee_vita_normalize_trophy_lottery_fields(
                   &late, flags, TROPHY_COUNT) == MELEE_VITA_SAVE_INVALID);
    assert(late.current_coins == 0x54260000);

    assert(melee_vita_normalize_trophy_lottery_fields(
                   &early, flags, TROPHY_COUNT) == MELEE_VITA_SAVE_BIG_ENDIAN);
    early.trophy_categories |= 4;
    assert(melee_vita_normalize_trophy_lottery_fields(
                   &early, flags, TROPHY_COUNT) == MELEE_VITA_SAVE_NATIVE);
    assert(early.trophy_count == TROPHY_COUNT);
    assert(early.trophy_categories == 0x00F4);
    assert(early.current_coins == 9812);
    assert(early.current_coins / 10U == 981);
}

static void test_ambiguous_and_invalid_data_is_untouched(void)
{
    uint16_t flags[TROPHY_COUNT] = { 0 };
    MeleeVitaTrophyLotteryFields ambiguous = { 0 };
    MeleeVitaTrophyLotteryFields invalid = {
        UINT16_MAX,
        UINT16_MAX,
        UINT32_MAX,
        0x12345678U,
        0x87654321U,
    };
    MeleeVitaTrophyLotteryFields original = invalid;
    MeleeVitaTrophyLotteryFields malformed_coins = {
        TROPHY_COUNT,
        0x00F4,
        10000,
        0x12345678U,
        0x87654321U,
    };
    MeleeVitaTrophyLotteryFields malformed_coins_original = malformed_coins;

    assert(melee_vita_normalize_trophy_lottery_fields(
               &ambiguous, flags, TROPHY_COUNT) == MELEE_VITA_SAVE_AMBIGUOUS);
    assert(melee_vita_normalize_trophy_lottery_fields(
               &invalid, flags, TROPHY_COUNT) == MELEE_VITA_SAVE_INVALID);
    assert(memcmp(&invalid, &original, sizeof(invalid)) == 0);
    assert(melee_vita_normalize_trophy_lottery_fields(
               &malformed_coins, flags,
               TROPHY_COUNT) == MELEE_VITA_SAVE_INVALID);
    assert(memcmp(&malformed_coins, &malformed_coins_original,
                  sizeof(malformed_coins)) == 0);
}

static void assert_bounded_digits(unsigned int value, size_t capacity)
{
    struct {
        int before;
        int digits[4];
        int after;
    } guarded = { 0x13579BDF, { -1, -1, -1, -1 }, 0x2468ACE };
    size_t count =
        tyFigupon_StoreDigits(guarded.digits, capacity, value);

    assert(count <= capacity);
    assert(guarded.before == 0x13579BDF);
    assert(guarded.after == 0x2468ACE);
}

static void test_decimal_digits_are_capacity_bounded(void)
{
    int digits[4] = { -1, -1, -1, -1 };

    assert(tyFigupon_StoreDigits(digits, 4, 0) == 1);
    assert(digits[0] == 0);
    assert(tyFigupon_StoreDigits(digits, 4, 999) == 3);
    assert(digits[0] == 9 && digits[1] == 9 && digits[2] == 9);

    assert_bounded_digits(141177651U, 3);
    assert_bounded_digits(141177651U, 4);
    assert_bounded_digits(UINT_MAX, 3);
    assert_bounded_digits(UINT_MAX, 4);
}

static void test_malformed_coins_fail_closed(void)
{
    assert(tyFigupon_ValidatedCoinTotal(0) == 0);
    assert(tyFigupon_ValidatedCoinTotal(9812) == 9812);
    assert(tyFigupon_ValidatedCoinTotal(9999) == 9999);
    assert(tyFigupon_ValidatedCoinTotal(10000) == 0);
    assert(tyFigupon_ValidatedCoinTotal(UINT_MAX) == 0);
}

int main(void)
{
    test_native_data_is_unchanged();
    test_reporter_tuple_is_normalized_once();
    test_gallery_normalized_reporter_state_recovers_coins();
    test_normalization_precedes_native_trophy_mutation();
    test_ambiguous_and_invalid_data_is_untouched();
    test_decimal_digits_are_capacity_bounded();
    test_malformed_coins_fail_closed();
    return 0;
}
