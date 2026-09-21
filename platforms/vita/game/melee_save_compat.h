#ifndef MELEE_VITA_SAVE_COMPAT_H
#define MELEE_VITA_SAVE_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MELEE_VITA_MAX_LOTTERY_COINS 0x270FU
#define MELEE_VITA_MAX_TROPHIES 293U
#define MELEE_VITA_TROPHY_CATEGORY_MASK 0x01FFU

typedef enum MeleeVitaSaveByteOrder {
    MELEE_VITA_SAVE_NATIVE,
    MELEE_VITA_SAVE_BIG_ENDIAN,
    MELEE_VITA_SAVE_AMBIGUOUS,
    MELEE_VITA_SAVE_INVALID,
} MeleeVitaSaveByteOrder;

typedef struct MeleeVitaTrophyLotteryFields {
    uint16_t trophy_count;
    uint16_t trophy_categories;
    uint32_t current_coins;
    uint32_t lifetime_coins;
    uint32_t session_coins;
} MeleeVitaTrophyLotteryFields;

static inline uint16_t melee_vita_swap_u16(uint16_t value)
{
    return (uint16_t) ((value << 8) | (value >> 8));
}

static inline uint32_t melee_vita_swap_u32(uint32_t value)
{
    return (value << 24) | ((value << 8) & 0x00FF0000U) |
           ((value >> 8) & 0x0000FF00U) | (value >> 24);
}

static inline MeleeVitaSaveByteOrder
melee_vita_classify_trophy_fields(uint16_t trophy_count,
                                  uint16_t trophy_categories)
{
    bool native_valid =
        trophy_count <= MELEE_VITA_MAX_TROPHIES &&
        (trophy_categories & ~MELEE_VITA_TROPHY_CATEGORY_MASK) == 0;
    bool big_endian_valid =
        melee_vita_swap_u16(trophy_count) <= MELEE_VITA_MAX_TROPHIES &&
        (melee_vita_swap_u16(trophy_categories) &
         ~MELEE_VITA_TROPHY_CATEGORY_MASK) == 0;

    if (native_valid && big_endian_valid) {
        return MELEE_VITA_SAVE_AMBIGUOUS;
    }
    if (native_valid) {
        return MELEE_VITA_SAVE_NATIVE;
    }
    if (big_endian_valid) {
        return MELEE_VITA_SAVE_BIG_ENDIAN;
    }
    return MELEE_VITA_SAVE_INVALID;
}

static inline MeleeVitaSaveByteOrder
melee_vita_classify_lottery_coins(uint32_t current_coins)
{
    bool native_valid = current_coins <= MELEE_VITA_MAX_LOTTERY_COINS;
    bool big_endian_valid =
        melee_vita_swap_u32(current_coins) <= MELEE_VITA_MAX_LOTTERY_COINS;

    if (native_valid && big_endian_valid) {
        return MELEE_VITA_SAVE_AMBIGUOUS;
    }
    if (native_valid) {
        return MELEE_VITA_SAVE_NATIVE;
    }
    if (big_endian_valid) {
        return MELEE_VITA_SAVE_BIG_ENDIAN;
    }
    return MELEE_VITA_SAVE_INVALID;
}

static inline MeleeVitaSaveByteOrder
melee_vita_normalize_trophy_lottery_fields(
    MeleeVitaTrophyLotteryFields* fields, uint16_t* trophy_flags,
    size_t trophy_flag_count)
{
    /* Older Vita builds normalized trophy data without converting coins, so
     * classify the two independently before changing either domain. */
    MeleeVitaSaveByteOrder trophy_order = melee_vita_classify_trophy_fields(
        fields->trophy_count, fields->trophy_categories);
    MeleeVitaSaveByteOrder coin_order =
        melee_vita_classify_lottery_coins(fields->current_coins);
    size_t i;

    if (trophy_order == MELEE_VITA_SAVE_INVALID ||
        coin_order == MELEE_VITA_SAVE_INVALID)
    {
        return MELEE_VITA_SAVE_INVALID;
    }

    if (trophy_order == MELEE_VITA_SAVE_BIG_ENDIAN) {
        fields->trophy_count = melee_vita_swap_u16(fields->trophy_count);
        fields->trophy_categories =
            melee_vita_swap_u16(fields->trophy_categories);
        for (i = 0; i < trophy_flag_count; i++) {
            trophy_flags[i] = melee_vita_swap_u16(trophy_flags[i]);
        }
    }
    if (coin_order == MELEE_VITA_SAVE_BIG_ENDIAN) {
        fields->current_coins = melee_vita_swap_u32(fields->current_coins);
        fields->lifetime_coins = melee_vita_swap_u32(fields->lifetime_coins);
        fields->session_coins = melee_vita_swap_u32(fields->session_coins);
    }

    if (trophy_order == MELEE_VITA_SAVE_BIG_ENDIAN ||
        coin_order == MELEE_VITA_SAVE_BIG_ENDIAN)
    {
        return MELEE_VITA_SAVE_BIG_ENDIAN;
    }
    if (trophy_order == MELEE_VITA_SAVE_AMBIGUOUS &&
        coin_order == MELEE_VITA_SAVE_AMBIGUOUS)
    {
        return MELEE_VITA_SAVE_AMBIGUOUS;
    }
    return MELEE_VITA_SAVE_NATIVE;
}

#endif
