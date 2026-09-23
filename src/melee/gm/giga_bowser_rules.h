#ifndef MELEE_GM_GIGA_BOWSER_RULES_H
#define MELEE_GM_GIGA_BOWSER_RULES_H

#include <stdbool.h>
#include <stdint.h>

enum {
    GM_GIGA_BOWSER_EVENT_INDEX = 50,
    GM_GIGA_BOWSER_SAVE_FLAG_UNLOCKED = 1 << 0,
    GM_GIGA_BOWSER_NOTIFICATION_ID = 0x42,
    GM_GIGA_BOWSER_CSS_CKIND = 0x1D,
    GM_GIGA_BOWSER_FIGHTER_KIND = 0x1F,
    GM_GIGA_BOWSER_BOWSER_CKIND = 5,
};

static inline bool gmGigaBowser_IsUnlocked(uint8_t giga_bowser_flags)
{
    return (giga_bowser_flags & GM_GIGA_BOWSER_SAVE_FLAG_UNLOCKED) != 0;
}

static inline bool gmGigaBowser_IsEligible(uint64_t completed_events,
                                           bool adventure_hard_completed,
                                           uint8_t giga_bowser_flags)
{
    return !gmGigaBowser_IsUnlocked(giga_bowser_flags) &&
           (((completed_events &
              (UINT64_C(1) << GM_GIGA_BOWSER_EVENT_INDEX)) != 0) ||
            adventure_hard_completed);
}

static inline uint8_t
gmGigaBowser_ApplyChallengerResult(uint8_t giga_bowser_flags, bool human_won)
{
    if (human_won) {
        giga_bowser_flags |= GM_GIGA_BOWSER_SAVE_FLAG_UNLOCKED;
    }
    return giga_bowser_flags;
}

static inline bool gmGigaBowser_IsCssVisible(uint8_t match_type,
                                             uint8_t giga_bowser_flags)
{
    bool const supported_mode = match_type <= 0x0D || (match_type >= 0x0F && match_type <= 0x17);
    return supported_mode && gmGigaBowser_IsUnlocked(giga_bowser_flags);
}

/* Brinstar's escape demo has no Giga fighter animation archive. */
static inline uint8_t gmGigaBowser_AdventureCutsceneKind(uint8_t scene_id,
                                                         uint8_t ckind)
{
    return scene_id == 0x1A && ckind == GM_GIGA_BOWSER_CSS_CKIND ? GM_GIGA_BOWSER_BOWSER_CKIND : ckind;
}

static inline uint8_t gmGigaBowser_FighterKindForCssSelection(uint8_t ckind)
{
    return ckind == GM_GIGA_BOWSER_CSS_CKIND
               ? GM_GIGA_BOWSER_FIGHTER_KIND
               : UINT8_MAX;
}

#endif
