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

static inline uint64_t melee_vita_movie_total_ticks(
    uint32_t frame_count, const uint32_t* rate_table)
{
    uint64_t total = 0u;
    uint32_t frame = 0u;

    if (rate_table == NULL) return frame_count;
    while (frame < frame_count) {
        const uint32_t count = rate_table[0];
        const uint32_t ticks = rate_table[1];
        if (count == 0u || ticks == 0u)
            return total + frame_count - frame;
        const uint32_t remaining = frame_count - frame;
        const uint32_t segment = count < remaining ? count : remaining;
        total += (uint64_t) segment * ticks;
        frame += segment;
        rate_table += 2;
    }
    return total;
}

static inline uint32_t melee_vita_movie_frame_for_tick(
    uint64_t tick, uint32_t frame_count, const uint32_t* rate_table)
{
    uint32_t frame = 0u;

    if (frame_count == 0u) return 0u;
    if (rate_table == NULL)
        return tick < frame_count ? (uint32_t) tick : frame_count - 1u;
    while (frame < frame_count) {
        const uint32_t count = rate_table[0];
        const uint32_t ticks = rate_table[1];
        if (count == 0u || ticks == 0u) {
            const uint64_t fallback = frame + tick;
            return fallback < frame_count ? (uint32_t) fallback
                                          : frame_count - 1u;
        }
        const uint32_t remaining = frame_count - frame;
        const uint32_t segment = count < remaining ? count : remaining;
        const uint64_t segment_ticks = (uint64_t) segment * ticks;
        if (tick < segment_ticks)
            return frame + (uint32_t) (tick / ticks);
        tick -= segment_ticks;
        frame += segment;
        rate_table += 2;
    }
    return frame_count - 1u;
}

static inline uint32_t melee_vita_movie_frame_ticks(
    uint32_t frame, const uint32_t* rate_table)
{
    uint32_t first = 0u;

    if (rate_table == NULL) return 1u;
    for (;;) {
        const uint32_t count = rate_table[0];
        const uint32_t ticks = rate_table[1];
        if (count == 0u || ticks == 0u) return 1u;
        if (frame - first < count) return ticks;
        first += count;
        rate_table += 2;
    }
}

static inline uint64_t melee_vita_opening_total_ticks(uint32_t frame_count)
{
    static const uint32_t rate_table[] = {
        MELEE_VITA_OPENING_FIRST_RATE_FRAMES, 2u,
        MELEE_VITA_OPENING_FAST_RATE_FRAMES, 1u,
        UINT32_MAX, 2u,
    };
    return melee_vita_movie_total_ticks(frame_count, rate_table);
}

static inline uint32_t melee_vita_opening_frame_for_tick(
    uint64_t tick, uint32_t frame_count)
{
    static const uint32_t rate_table[] = {
        MELEE_VITA_OPENING_FIRST_RATE_FRAMES, 2u,
        MELEE_VITA_OPENING_FAST_RATE_FRAMES, 1u,
        UINT32_MAX, 2u,
    };
    return melee_vita_movie_frame_for_tick(tick, frame_count, rate_table);
}

static inline uint32_t melee_vita_opening_frame_ticks(uint32_t frame)
{
    static const uint32_t rate_table[] = {
        MELEE_VITA_OPENING_FIRST_RATE_FRAMES, 2u,
        MELEE_VITA_OPENING_FAST_RATE_FRAMES, 1u,
        UINT32_MAX, 2u,
    };
    return melee_vita_movie_frame_ticks(frame, rate_table);
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
