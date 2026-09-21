#ifndef MELEE_VITA_RESULTS_BACKDROP_H
#define MELEE_VITA_RESULTS_BACKDROP_H

#include <stdint.h>

enum {
    MELEE_VITA_RESULTS_CAPTURE_PRIORITY = 0,
    MELEE_VITA_RESULTS_BACKDROP_PRIORITY = 4,
    MELEE_VITA_RESULTS_WINNER_PRIORITY = 5,
    MELEE_VITA_RESULTS_PANEL_PRIORITY = 8,
};

typedef struct MeleeVitaResultsBackdropState {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t alpha;
    uint8_t write_color;
    uint8_t write_alpha;
    uint8_t write_depth;
    uint8_t render_priority;
} MeleeVitaResultsBackdropState;

static inline MeleeVitaResultsBackdropState
melee_vita_results_backdrop_state(void)
{
    const MeleeVitaResultsBackdropState state = {
        0, 0, 0, 0, 1, 0, 0, MELEE_VITA_RESULTS_BACKDROP_PRIORITY,
    };
    return state;
}

#endif
