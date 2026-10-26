#include "random.h"

static u32 seed = 1;
u32* HSD_RandSeedPtr = &seed;

#ifdef MELEE_VITA_SLIPPI
/* The Slippi replay test lists who called the RNG in a frame whose seed
 * came out wrong (src/melee/slippi/slippi_replay.c). */
void mp_slippi_replay_note_rng(void* caller);
#define NOTE_RNG_CALLER() mp_slippi_replay_note_rng(__builtin_return_address(0))
#else
#define NOTE_RNG_CALLER()
#endif

s32 HSD_Rand(void)
{
    NOTE_RNG_CALLER();
    *HSD_RandSeedPtr = *HSD_RandSeedPtr * 214013 + 2531011;
    return *HSD_RandSeedPtr >> 0x10;
}

f32 HSD_Randf(void)
{
    NOTE_RNG_CALLER();
    *HSD_RandSeedPtr = *HSD_RandSeedPtr * 214013 + 2531011;
    return (f32) (*HSD_RandSeedPtr >> 0x10) / (1 << 16);
}

s32 HSD_Randi(s32 max_val)
{
#ifdef MELEE_VITA_SLIPPI
    /* The same as max_val * HSD_Rand() / (1 << 16), but done here, so the
     * caller noted is the one that called HSD_Randi. */
    s32 value;
    NOTE_RNG_CALLER();
    *HSD_RandSeedPtr = *HSD_RandSeedPtr * 214013 + 2531011;
    value = *HSD_RandSeedPtr >> 0x10;
    return max_val * value / (1 << 16);
#else
    return max_val * HSD_Rand() / (1 << 16);
#endif
}

void _HSD_RandForgetMemory(void* low, void* high)
{
    if (low <= (void*) HSD_RandSeedPtr && (void*) HSD_RandSeedPtr < high) {
        HSD_RandSeedPtr = &seed;
    }
    return;
}
