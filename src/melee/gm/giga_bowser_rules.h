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
    GM_GIGA_BOWSER_RECORD_MAGIC = 0x47425243, /* GBRC */
    GM_GIGA_BOWSER_RECORD_VERSION = 1,
    GM_GIGA_BOWSER_RECORD_INDEX = 25,
};

static inline uint8_t gmGigaBowser_RecordIndex(uint8_t ckind,
                                               uint8_t legacy_selkind)
{
    return ckind == GM_GIGA_BOWSER_CSS_CKIND
               ? GM_GIGA_BOWSER_RECORD_INDEX
               : legacy_selkind;
}

static inline bool gmGigaBowser_RecordNeedsInitialization(uint32_t magic,
                                                           uint32_t version)
{
    return magic != GM_GIGA_BOWSER_RECORD_MAGIC ||
           version != GM_GIGA_BOWSER_RECORD_VERSION;
}

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
    bool const supported_mode = match_type <= 0x17;
    return supported_mode && gmGigaBowser_IsUnlocked(giga_bowser_flags);
}

/* Adventure cutscenes animate the selected fighter with character-specific
 * archives that do not contain Giga Bowser's animations. */
static inline uint8_t gmGigaBowser_AdventureCutsceneKind(uint8_t scene_id,
                                                         uint8_t ckind)
{
    if (ckind != GM_GIGA_BOWSER_CSS_CKIND) {
        return ckind;
    }
    switch (scene_id) {
    case 0x1A: /* Brinstar escape */
    case 0x22: /* Team Kirby introduction */
    case 0x24: /* Giant Kirby introduction */
    case 0x52: /* Metal Mario/Luigi introduction */
    case 0x5A: /* Bowser toy defeat */
    case 0x5B: /* Bowser-to-Giga transformation */
    case 0x5D: /* Giga Bowser defeat */
        return GM_GIGA_BOWSER_BOWSER_CKIND;
    default:
        return ckind;
    }
}

static inline uint8_t gmGigaBowser_AdventureCutsceneColor(uint8_t scene_id,
                                                          uint8_t ckind,
                                                          uint8_t color)
{
    return gmGigaBowser_AdventureCutsceneKind(scene_id, ckind) != ckind ? 0 : color;
}

static inline uint8_t gmGigaBowser_FighterKindForCssSelection(uint8_t ckind)
{
    return ckind == GM_GIGA_BOWSER_CSS_CKIND
               ? GM_GIGA_BOWSER_FIGHTER_KIND
               : UINT8_MAX;
}

#endif
