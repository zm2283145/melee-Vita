#include <assert.h>

#include "results_backdrop.h"

int main(void)
{
    const MeleeVitaResultsBackdropState state =
        melee_vita_results_backdrop_state();

    assert(state.red == 0);
    assert(state.green == 0);
    assert(state.blue == 0);
    assert(state.write_color == 1);
    assert(state.write_alpha == 0);
    assert(state.write_depth == 0);
    assert(state.render_priority > MELEE_VITA_RESULTS_CAPTURE_PRIORITY);
    assert(state.render_priority < MELEE_VITA_RESULTS_WINNER_PRIORITY);
    assert(MELEE_VITA_RESULTS_WINNER_PRIORITY <
           MELEE_VITA_RESULTS_PANEL_PRIORITY);
    return 0;
}
