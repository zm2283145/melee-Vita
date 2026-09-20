#include <assert.h>

#include "opening_timeline.h"

int main(void)
{
    MeleeVitaOpeningPresentation presentation;

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
