/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PS5 system services used by the port (user, pad, audio, notifications). */
#ifndef MELEE_PS5_SERVICES_H
#define MELEE_PS5_SERVICES_H

#include <stdbool.h>
#include <stdint.h>

#define MELEE_PS5_MAX_PADS 4

/* Buttons use the Vita SCE_CTRL_* bit layout (the PS5 layout is the same,
 * except the touch pad, which reports as SELECT). Sticks: 0..255, 128 centre. */
typedef struct MeleePs5PadState {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;
    uint8_t l2, r2;
} MeleePs5PadState;

int melee_ps5_services_init(void);
/* Reads pad `index` (0..3, in login order). False if it is not connected. */
bool melee_ps5_pad_read(int index, MeleePs5PadState* state);
int melee_ps5_audio_open(uint32_t frames_per_grain);
int melee_ps5_audio_output(int handle, const int16_t* stereo);
void melee_ps5_audio_close(int handle);
void melee_ps5_notify(const char* message);

#endif
