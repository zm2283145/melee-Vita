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

#endif
