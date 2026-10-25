/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MELEE_VITA_PAD_VITA_H
#define MELEE_VITA_PAD_VITA_H

#include "pad_mapping.h"

#include <stdint.h>

uint32_t melee_vita_pad_raw_buttons_triggered(void);
uint32_t melee_vita_pad_raw_buttons_held(void);
uint32_t melee_vita_pad_primary_shoulder_mask(void);
bool melee_vita_pad_touch_triggered(int* x, int* y);
int melee_vita_pad_get_mapping(int physical_button);
int melee_vita_pad_get_physical_button_for_action(int action);
int melee_vita_pad_physical_button_from_raw(uint32_t raw_buttons);
bool melee_vita_pad_set_mapping(int physical_button, int action);
void melee_vita_pad_reset_mapping(void);
bool melee_vita_pad_replace_mapping(const uint8_t mapping[MELEE_VITA_BUTTON_COUNT]);

/* Stick calibration from ux0:data/melee/sticks.txt (LiveArea Settings >
 * Controls). Raw Vita units, 0-255. dead is a percent of full tilt. */
typedef struct MeleeVitaStickCal {
    int center_x, center_y;
    int min_x, max_x, min_y, max_y;
    int dead;
} MeleeVitaStickCal;
enum { MELEE_VITA_STICK_LEFT, MELEE_VITA_STICK_RIGHT, MELEE_VITA_STICK_COUNT };
void melee_vita_pad_stick_cal_defaults(MeleeVitaStickCal* cal);
void melee_vita_pad_get_stick_cal(int stick, MeleeVitaStickCal* cal);
bool melee_vita_pad_replace_stick_cal(const MeleeVitaStickCal cal[MELEE_VITA_STICK_COUNT]);
/* raw sample -> GameCube stick values (-127..127, up is positive) */
void melee_vita_pad_stick_apply(const MeleeVitaStickCal* cal, int raw_x, int raw_y,
                                int8_t* x, int8_t* y);

#endif
