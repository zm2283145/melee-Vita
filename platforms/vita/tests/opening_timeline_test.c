#include <assert.h>

#include "opening_timeline.h"

int main(void)
{
    MeleeVitaOpeningPresentation presentation;
    static const uint32_t varied_rate_table[] = {
        1u, 19u,
        3u, 1u,
        2u, 5u,
        UINT32_MAX, 2u,
    };

    assert(melee_vita_opening_total_ticks(0u) == 0u);
    assert(melee_vita_opening_total_ticks(1250u) == 2500u);
    assert(melee_vita_opening_total_ticks(1644u) == 2894u);
    assert(melee_vita_opening_total_ticks(2000u) == 3606u);

    assert(melee_vita_opening_frame_for_tick(0u, 2000u) == 0u);
    assert(melee_vita_opening_frame_for_tick(2499u, 2000u) == 1249u);
    assert(melee_vita_opening_frame_for_tick(2500u, 2000u) == 1250u);
    assert(melee_vita_opening_frame_for_tick(2893u, 2000u) == 1643u);
    assert(melee_vita_opening_frame_for_tick(2894u, 2000u) == 1644u);
    assert(melee_vita_opening_frame_for_tick(3605u, 2000u) == 1999u);
    assert(melee_vita_opening_frame_for_tick(3606u, 2000u) == 1999u);
    assert(melee_vita_opening_frame_ticks(1249u) == 2u);
    assert(melee_vita_opening_frame_ticks(1250u) == 1u);
    assert(melee_vita_opening_frame_ticks(1643u) == 1u);
    assert(melee_vita_opening_frame_ticks(1644u) == 2u);

    assert(melee_vita_movie_total_ticks(0u, varied_rate_table) == 0u);
    assert(melee_vita_movie_total_ticks(8u, varied_rate_table) == 36u);
    assert(melee_vita_movie_frame_for_tick(0u, 8u, varied_rate_table) == 0u);
    assert(melee_vita_movie_frame_for_tick(18u, 8u, varied_rate_table) == 0u);
    assert(melee_vita_movie_frame_for_tick(19u, 8u, varied_rate_table) == 1u);
    assert(melee_vita_movie_frame_for_tick(21u, 8u, varied_rate_table) == 3u);
    assert(melee_vita_movie_frame_for_tick(22u, 8u, varied_rate_table) == 4u);
    assert(melee_vita_movie_frame_for_tick(31u, 8u, varied_rate_table) == 5u);
    assert(melee_vita_movie_frame_for_tick(32u, 8u, varied_rate_table) == 6u);
    assert(melee_vita_movie_frame_for_tick(36u, 8u, varied_rate_table) == 7u);
    assert(melee_vita_movie_frame_ticks(0u, varied_rate_table) == 19u);
    assert(melee_vita_movie_frame_ticks(1u, varied_rate_table) == 1u);
    assert(melee_vita_movie_frame_ticks(4u, varied_rate_table) == 5u);
    assert(melee_vita_movie_frame_ticks(6u, varied_rate_table) == 2u);

    assert(melee_vita_movie_total_ticks(8u, NULL) == 8u);
    assert(melee_vita_movie_frame_for_tick(0u, 8u, NULL) == 0u);
    assert(melee_vita_movie_frame_for_tick(7u, 8u, NULL) == 7u);
    assert(melee_vita_movie_frame_for_tick(8u, 8u, NULL) == 7u);
    assert(melee_vita_movie_frame_ticks(0u, NULL) == 1u);
    assert(melee_vita_movie_frame_ticks(7u, NULL) == 1u);

    melee_vita_opening_presentation_start(&presentation);
    assert(!presentation.playback_finished);
    assert(presentation.visible);
    assert(melee_vita_opening_presentation_finish(&presentation));
    assert(presentation.playback_finished);
    assert(presentation.visible);
    assert(!melee_vita_opening_presentation_finish(&presentation));
    melee_vita_opening_presentation_hide(&presentation);
    assert(!presentation.visible);
    melee_vita_opening_presentation_hide(&presentation);
    assert(!presentation.visible);

    melee_vita_opening_presentation_start(&presentation);
    melee_vita_opening_presentation_hide(&presentation);
    assert(!presentation.playback_finished);
    assert(!presentation.visible);
    return 0;
}
