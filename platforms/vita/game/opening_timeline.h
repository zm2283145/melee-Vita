#ifndef MELEE_VITA_OPENING_TIMELINE_H
#define MELEE_VITA_OPENING_TIMELINE_H

#include <stdbool.h>
#include <stdint.h>

#define MELEE_VITA_OPENING_FIRST_RATE_FRAMES 1250u
#define MELEE_VITA_OPENING_FAST_RATE_FRAMES 394u

typedef struct MeleeVitaOpeningPresentation {
    bool playback_finished;
    bool visible;
} MeleeVitaOpeningPresentation;

static inline uint64_t melee_vita_opening_total_ticks(uint32_t frame_count)
{
    const uint32_t first =
        frame_count < MELEE_VITA_OPENING_FIRST_RATE_FRAMES
        ? frame_count : MELEE_VITA_OPENING_FIRST_RATE_FRAMES;
    const uint32_t after_first = frame_count - first;
    const uint32_t fast =
        after_first < MELEE_VITA_OPENING_FAST_RATE_FRAMES
        ? after_first : MELEE_VITA_OPENING_FAST_RATE_FRAMES;
    const uint32_t final = after_first - fast;
    return (uint64_t) first * 2u + fast + (uint64_t) final * 2u;
}

static inline uint32_t melee_vita_opening_frame_for_tick(
    uint64_t tick, uint32_t frame_count)
{
    uint64_t frame;
    const uint64_t first_ticks =
        (uint64_t) MELEE_VITA_OPENING_FIRST_RATE_FRAMES * 2u;

    if (frame_count == 0u) return 0u;
    if (tick < first_ticks) {
        frame = tick / 2u;
    } else if (tick - first_ticks < MELEE_VITA_OPENING_FAST_RATE_FRAMES) {
        frame = MELEE_VITA_OPENING_FIRST_RATE_FRAMES + tick - first_ticks;
    } else {
        frame = MELEE_VITA_OPENING_FIRST_RATE_FRAMES +
                MELEE_VITA_OPENING_FAST_RATE_FRAMES +
                (tick - first_ticks -
                 MELEE_VITA_OPENING_FAST_RATE_FRAMES) /
                    2u;
    }
    return frame < frame_count ? (uint32_t) frame : frame_count - 1u;
}

static inline uint32_t melee_vita_opening_frame_ticks(uint32_t frame)
{
    return frame >= MELEE_VITA_OPENING_FIRST_RATE_FRAMES &&
                   frame < MELEE_VITA_OPENING_FIRST_RATE_FRAMES +
                               MELEE_VITA_OPENING_FAST_RATE_FRAMES
        ? 1u : 2u;
}

static inline void melee_vita_opening_presentation_start(
    MeleeVitaOpeningPresentation* presentation)
{
    presentation->playback_finished = false;
    presentation->visible = true;
}

static inline bool melee_vita_opening_presentation_finish(
    MeleeVitaOpeningPresentation* presentation)
{
    if (presentation->playback_finished) return false;
    presentation->playback_finished = true;
    return true;
}

static inline void melee_vita_opening_presentation_hide(
    MeleeVitaOpeningPresentation* presentation)
{
    presentation->visible = false;
}

#endif
